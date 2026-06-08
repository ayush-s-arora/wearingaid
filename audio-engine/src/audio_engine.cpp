#include "audio_engine.h"
#include "key_profiles.h"
#include "pffft.h"
#include <Eigen/Dense>
#include <iostream>
#include <cmath>
#include <algorithm>

using namespace AudioEngine;

class HmmAudioEngine {
private:
    Eigen::MatrixXd key_profiles;          
    Eigen::MatrixXd log_transition_matrix; 
    Eigen::VectorXd log_viterbi_path;      
    
    uint32_t active_features;

    int current_estimated_key;
    float tracked_bpm;
    float time_since_last_beat;

    void build_key_profiles(ProfileType type) {
        const double* maj_ptr = nullptr;
        const double* min_ptr = nullptr;

        switch (type) {
            case ProfileType::TEMPERLEY:
                maj_ptr = Profiles::TEMPERLEY_MAJ.data();
                min_ptr = Profiles::TEMPERLEY_MIN.data();
                std::cout << "[Engine] Matrix Rebuilt: TEMPERLEY (Classical)" << std::endl;
                break;
            case ProfileType::EDMA:
                maj_ptr = Profiles::EDMA_MAJ.data();
                min_ptr = Profiles::EDMA_MIN.data();
                std::cout << "[Engine] Matrix Rebuilt: EDMA (Electronic)" << std::endl;
                break;
            case ProfileType::WEI_CHAI:
                maj_ptr = Profiles::WEI_CHAI_MAJ.data();
                min_ptr = Profiles::WEI_CHAI_MIN.data();
                std::cout << "[Engine] Matrix Rebuilt: WEI CHAI (Acoustic/Folk)" << std::endl;
                break;
            case ProfileType::TONIC_TRIAD:
                maj_ptr = Profiles::TONIC_TRIAD_MAJ.data();
                min_ptr = Profiles::TONIC_TRIAD_MIN.data();
                std::cout << "[Engine] Matrix Rebuilt: TONIC TRIAD (Minimalist)" << std::endl;
                break;
            case ProfileType::SHAATH:
            default:
                maj_ptr = Profiles::SHAATH_MAJ.data();
                min_ptr = Profiles::SHAATH_MIN.data();
                std::cout << "[Engine] Matrix Rebuilt: SHAATH (Pop/Rock/Jazz)" << std::endl;
                break;
        }

        // Circularly shift the 12-bin vector across all 24 keys into the HMM matrix
        // bin 0 is the root (C)
        for (int key = 0; key < 12; ++key) {
            for (int chroma_bin = 0; chroma_bin < 12; ++chroma_bin) {
                int shifted_bin = (chroma_bin - key + 12) % 12;
                key_profiles(key, chroma_bin) = maj_ptr[shifted_bin];         // Rows 0-11
                key_profiles(key + 12, chroma_bin) = min_ptr[shifted_bin];    // Rows 12-23
            }
        }
    }

    void initialize_matrices() {
        key_profiles = Eigen::MatrixXd::Zero(24, 12);
        log_transition_matrix = Eigen::MatrixXd::Zero(24, 24);
        log_viterbi_path = Eigen::VectorXd::Constant(24, std::log(1.0 / 24.0));

        // Default to the contemporary band profile
        build_key_profiles(ProfileType::SHAATH);

        // Inertial Transition flywheel (85% hold probability)
        for (int i = 0; i < 24; ++i) {
            for (int j = 0; j < 24; ++j) {
                log_transition_matrix(i, j) = (i == j) ? std::log(0.85) : std::log(0.15 / 23.0);
            }
        }
    }

public:
    HmmAudioEngine(uint32_t features) : active_features(features) {
        initialize_matrices();
        current_estimated_key = 0;
        tracked_bpm = 120.0f;
        time_since_last_beat = 0.0f;
    }

    void set_features(uint32_t features) {
        active_features = features;
    }

    void process_audio(const float* pcm_data, int num_samples) {
        float* aligned_in = (float*)pffft_aligned_malloc(num_samples * sizeof(float));
        float* aligned_out = (float*)pffft_aligned_malloc(num_samples * sizeof(float));
        float* work_buffer = (float*)pffft_aligned_malloc(num_samples * sizeof(float));

        std::copy(pcm_data, pcm_data + num_samples, aligned_in);

        PFFFT_Setup* setup = pffft_new_setup(num_samples, PFFFT_REAL);
        pffft_transform_ordered(setup, aligned_in, aligned_out, work_buffer, PFFFT_FORWARD);

        Eigen::VectorXf chroma = Eigen::VectorXf::Zero(12);
        float sample_rate = 44100.0f; 
        
        for (int i = 1; i < num_samples / 2; ++i) {
            float re = aligned_out[2 * i];
            float im = aligned_out[2 * i + 1];
            float magnitude = std::sqrt(re * re + im * im);
            float frequency = i * (sample_rate / num_samples);
            
            if (frequency > 50.0f && frequency < 4000.0f) { 
                int pitch = std::round(69 + 12 * std::log2(frequency / 440.0f)); // freq -> MIDI note
                int bin = ((pitch % 12) + 12) % 12; // pitch class, C = 0
                chroma(bin) += magnitude;
            }
        }

        pffft_destroy_setup(setup);
        pffft_aligned_free(aligned_in);
        pffft_aligned_free(aligned_out);
        pffft_aligned_free(work_buffer);

        chroma.array() += 1e-6; 
        chroma = chroma.array() / chroma.sum();
        
        Eigen::VectorXd log_emissions = (key_profiles * chroma.cast<double>()).array().log();
        Eigen::VectorXd next_viterbi(24);
        
        for (int j = 0; j < 24; ++j) {
            double max_val = -1e9;
            for (int i = 0; i < 24; ++i) {
                double val = log_viterbi_path(i) + log_transition_matrix(i, j);
                if (val > max_val) max_val = val;
            }
            next_viterbi(j) = max_val + log_emissions(j);
        }
        
        log_viterbi_path = next_viterbi.array() - next_viterbi.maxCoeff();
        log_viterbi_path.maxCoeff(&current_estimated_key);
    }

    void set_genre(ProfileType type) {
        build_key_profiles(type);
        // Reset the HMM path to prevent inertia from the previous genre dragging
        log_viterbi_path = Eigen::VectorXd::Constant(24, std::log(1.0 / 24.0));
    }

    EngineOutput tick_tempo(float time_delta_seconds) {
        time_since_last_beat += time_delta_seconds;
        return EngineOutput{current_estimated_key, tracked_bpm, 0};
    }
};

// --- C-API Implementations ---
struct EngineState { HmmAudioEngine* instance; };

EngineState* engine_create(uint32_t features) {
    return new EngineState{new HmmAudioEngine(features)}; 
}

void engine_push_audio(EngineState* engine, const float* pcm_data, int num_samples) {
    if (engine && engine->instance) engine->instance->process_audio(pcm_data, num_samples);
}

EngineOutput engine_tick_tempo(EngineState* engine, float time_delta_seconds) {
    if (engine && engine->instance) return engine->instance->tick_tempo(time_delta_seconds);
    return EngineOutput{0, 120.0f, 0};
}

void engine_set_genre(EngineState* engine, int genre_code) {
    if (engine && engine->instance) {
        // Safely bound the incoming int from Kotlin/JNI
        int safe_code = std::clamp(genre_code, 0, 4);
        engine->instance->set_genre(static_cast<ProfileType>(safe_code));
    }
}

void engine_set_features(EngineState* engine, uint32_t features) {
    if (engine && engine->instance) engine->instance->set_features(features);
}

void engine_destroy(EngineState* engine) {
    if (engine) { 
        delete engine->instance; 
        delete engine; 
    }
}