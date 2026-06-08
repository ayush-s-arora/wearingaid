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

    static constexpr int   TEMPO_BPM_MIN          = 60;
    static constexpr int   TEMPO_BPM_MAX          = 180;
    static constexpr int   TEMPO_BPM_STEP         = 2;
    static constexpr int   TEMPO_STATES           = (TEMPO_BPM_MAX - TEMPO_BPM_MIN) / TEMPO_BPM_STEP + 1; // 61
    static constexpr float MIN_ONSET_GAP_S        = 0.25f;   // prevents double-triggers, allows up to 240 BPM
    static constexpr float ONSET_THRESHOLD_RATIO  = 1.8f;    // onset if RMS > 1.8x rolling average
    static constexpr int   ENERGY_BUF_SIZE        = 43;      // ~1 second of history at 1024/44100

    Eigen::VectorXd tempo_log_viterbi;
    Eigen::MatrixXd tempo_log_transition;

    float current_time_s        = 0.0f;
    float last_onset_time_s     = -1.0f;
    float predicted_beat_time_s = -1.0f;
    bool  haptic_pending        = false;

    float energy_buf[ENERGY_BUF_SIZE] = {};
    int   energy_buf_idx              = 0;
    float energy_buf_sum              = 0.0f;

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

    void initialize_tempo() {
        tempo_log_viterbi = Eigen::VectorXd::Constant(TEMPO_STATES, std::log(1.0 / TEMPO_STATES));
        tempo_log_transition = Eigen::MatrixXd::Zero(TEMPO_STATES, TEMPO_STATES);

        // Normally distributed falloff, since neighboring BPMs are more likely than distant ones
        const double sigma = 2.0; // in state units = 4 BPM spread
        for (int i = 0; i < TEMPO_STATES; ++i) {
            double row_sum = 0.0;
            std::vector<double> row(TEMPO_STATES);
            for (int j = 0; j < TEMPO_STATES; ++j) {
                double d = j - i;
                row[j] = std::exp(-0.5 * (d / sigma) * (d / sigma));
                row_sum += row[j];
            }
            for (int j = 0; j < TEMPO_STATES; ++j)
                tempo_log_transition(i, j) = std::log(row[j] / row_sum);
        }
    }

    Eigen::VectorXd compute_tempo_emissions(float ioi_seconds) {
        Eigen::VectorXd log_emissions(TEMPO_STATES);
        const double sigma_ioi = 0.03; // 30ms timing tolerance (inter-onset interval)
        for (int s = 0; s < TEMPO_STATES; ++s) {
            float expected_ioi = 60.0f / (TEMPO_BPM_MIN + s * TEMPO_BPM_STEP);
            double diff = ioi_seconds - expected_ioi;
            log_emissions(s) = -0.5 * (diff / sigma_ioi) * (diff / sigma_ioi);
        }
        return log_emissions;
    }

    void update_tempo_viterbi(float ioi_seconds) {
        Eigen::VectorXd log_emissions = compute_tempo_emissions(ioi_seconds);
        Eigen::VectorXd next_viterbi(TEMPO_STATES);
        for (int j = 0; j < TEMPO_STATES; ++j) {
            double max_val = -1e9;
            for (int i = 0; i < TEMPO_STATES; ++i) {
                double val = tempo_log_viterbi(i) + tempo_log_transition(i, j);
                if (val > max_val) max_val = val;
            }
            next_viterbi(j) = max_val + log_emissions(j);
        }
        tempo_log_viterbi = next_viterbi.array() - next_viterbi.maxCoeff();
    }

    float get_estimated_bpm() {
        int best_state;
        tempo_log_viterbi.maxCoeff(&best_state);
        return static_cast<float>(TEMPO_BPM_MIN + best_state * TEMPO_BPM_STEP);
    }

    void initialize_matrices() {
        key_profiles = Eigen::MatrixXd::Zero(24, 12);
        log_transition_matrix = Eigen::MatrixXd::Zero(24, 24);
        log_viterbi_path = Eigen::VectorXd::Constant(24, std::log(1.0 / 24.0));

        // Default to the contemporary band profile
        build_key_profiles(ProfileType::SHAATH);
        initialize_tempo();

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

        if (active_features & WEARINGAID_FEATURE_TEMPO) {
            // RMS energy of this frame
            float rms_sq = 0.0f;
            for (int i = 0; i < num_samples; ++i) rms_sq += pcm_data[i] * pcm_data[i];
            float rms = std::sqrt(rms_sq / num_samples);

            // Adaptive rolling average
            energy_buf_sum -= energy_buf[energy_buf_idx];
            energy_buf[energy_buf_idx] = rms;
            energy_buf_sum += rms;
            energy_buf_idx = (energy_buf_idx + 1) % ENERGY_BUF_SIZE;
            float avg_energy = energy_buf_sum / ENERGY_BUF_SIZE;

            // Advance internal clock
            float frame_duration_s = static_cast<float>(num_samples) / 44100.0f;
            current_time_s += frame_duration_s;

            // Onset detection: loud spike above adaptive threshold with minimum gap
            bool is_onset = (avg_energy > 1e-6f) &&
                            (rms > avg_energy * ONSET_THRESHOLD_RATIO) &&
                            (current_time_s - last_onset_time_s > MIN_ONSET_GAP_S);

            if (is_onset) {
                if (last_onset_time_s > 0.0f) {
                    float ioi = current_time_s - last_onset_time_s;
                    float candidate_bpm = 60.0f / ioi;
                    if (candidate_bpm >= TEMPO_BPM_MIN && candidate_bpm <= TEMPO_BPM_MAX) {
                        update_tempo_viterbi(ioi);
                        tracked_bpm = get_estimated_bpm();
                        if (predicted_beat_time_s < 0.0f) {
                            predicted_beat_time_s = current_time_s + (60.0f / tracked_bpm);
                        }
                    }
                }
                last_onset_time_s = current_time_s;
            }

            // Fire haptic when predicted beat arrives
            if (predicted_beat_time_s > 0.0f && current_time_s >= predicted_beat_time_s) {
                haptic_pending = true;
                predicted_beat_time_s += 60.0f / tracked_bpm; // advance to next beat
            }
        }
    }

    void set_genre(ProfileType type) {
        build_key_profiles(type);
        // Reset the HMM path to prevent inertia from the previous genre dragging
        log_viterbi_path = Eigen::VectorXd::Constant(24, std::log(1.0 / 24.0));
    }

    EngineOutput tick_tempo(float time_delta_seconds) {
        time_since_last_beat += time_delta_seconds;

        int trigger = haptic_pending ? 1 : 0;
        haptic_pending = false;

        return EngineOutput{
            current_estimated_key,
            tracked_bpm,
            trigger
        };
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