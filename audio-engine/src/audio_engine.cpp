#include "audio_engine.h"
#include "key_profiles.h"
#include "pffft.h"
#include <Eigen/Dense>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <atomic>

using namespace AudioEngine;

class HmmAudioEngine {
private:
    Eigen::MatrixXd key_profiles;          
    Eigen::MatrixXd log_transition_matrix; 
    Eigen::VectorXd log_viterbi_path;      
    
    std::atomic<uint32_t> active_features{0};

    // Cross-thread values: written by process_audio (Oboe callback thread),
    // read by tick_tempo (Kotlin coroutine thread). std::atomic ensures no torn reads.
    std::atomic<int>   current_estimated_key{0};
    std::atomic<float> tracked_bpm{120.0f};
    std::atomic<bool>  haptic_pending{false};
    std::atomic<float> last_frame_rms{0.0f};

    float time_since_last_beat = 0.0f;

    static constexpr int   TEMPO_BPM_MIN          = 60;
    static constexpr int   TEMPO_BPM_MAX          = 180;
    static constexpr int   TEMPO_BPM_STEP         = 2;
    static constexpr int   TEMPO_STATES           = (TEMPO_BPM_MAX - TEMPO_BPM_MIN) / TEMPO_BPM_STEP + 1; // 61
    static constexpr float MIN_ONSET_GAP_S        = 0.25f;
    static constexpr float ONSET_THRESHOLD_RATIO  = 1.8f;
    static constexpr int   ENERGY_BUF_SIZE        = 43;      // ~1 s of history at 1024/44100

    Eigen::VectorXd tempo_log_viterbi;
    Eigen::MatrixXd tempo_log_transition;

    float current_time_s        = 0.0f;
    float last_onset_time_s     = -1.0f;
    float predicted_beat_time_s = -1.0f;

    float energy_buf[ENERGY_BUF_SIZE] = {};
    int   energy_buf_idx              = 0;
    float energy_buf_sum              = 0.0f;

    // Jiggle rejection: only update the key HMM after this many consecutive loud
    // frames. A jiggle burst (1–3 frames, ~25–70 ms) never reaches the threshold;
    // a music pause simply preserves the last HMM state without resetting it.
    int sustained_frames = 0;
    static constexpr int   MIN_SUSTAINED_FRAMES = 8;   // ~185 ms at 1024/44100; rejects wrist shakes
    static constexpr float SILENCE_THRESHOLD    = 0.05f; // ~-26 dBFS; well below band levels

    // Cached PFFFT resources; allocated once, reused every callback.
    static constexpr int EXPECTED_FRAME_SIZE = 1024;
    PFFFT_Setup* fft_setup = nullptr;
    float*       fft_in    = nullptr;
    float*       fft_out   = nullptr;
    float*       fft_work  = nullptr;

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
        fft_setup = pffft_new_setup(EXPECTED_FRAME_SIZE, PFFFT_REAL);
        fft_in    = (float*)pffft_aligned_malloc(EXPECTED_FRAME_SIZE * sizeof(float));
        fft_out   = (float*)pffft_aligned_malloc(EXPECTED_FRAME_SIZE * sizeof(float));
        fft_work  = (float*)pffft_aligned_malloc(EXPECTED_FRAME_SIZE * sizeof(float));
        initialize_matrices();
    }

    ~HmmAudioEngine() {
        pffft_destroy_setup(fft_setup);
        pffft_aligned_free(fft_in);
        pffft_aligned_free(fft_out);
        pffft_aligned_free(fft_work);
    }

    void set_features(uint32_t features) {
        active_features.store(features, std::memory_order_relaxed);
    }

    void process_audio(const float* pcm_data, int num_samples) {
        if (num_samples != EXPECTED_FRAME_SIZE) return;

        // FFT using cached setup. No allocation on the hot path
        std::copy(pcm_data, pcm_data + num_samples, fft_in);
        pffft_transform_ordered(fft_setup, fft_in, fft_out, fft_work, PFFFT_FORWARD);

        // RMS. Computed before the Viterbi update so the sustain gate can use it
        float rms_sq = 0.0f;
        for (int i = 0; i < num_samples; ++i) rms_sq += pcm_data[i] * pcm_data[i];
        const float rms = std::sqrt(rms_sq / num_samples);
        last_frame_rms.store(rms, std::memory_order_relaxed);

        // Sustain gate: require MIN_SUSTAINED_FRAMES consecutive loud frames before
        // updating the key HMM. This rejects brief jiggle transients (1–3 frames)
        // without resetting HMM state during normal music pauses.
        if (rms >= SILENCE_THRESHOLD) {
            sustained_frames = std::min(sustained_frames + 1, MIN_SUSTAINED_FRAMES);
        } else {
            sustained_frames = 0;
        }

        if ((active_features.load(std::memory_order_relaxed) & WEARINGAID_FEATURE_KEY) &&
            sustained_frames >= MIN_SUSTAINED_FRAMES) {
            Eigen::VectorXf chroma = Eigen::VectorXf::Zero(12);
            for (int i = 1; i < num_samples / 2; ++i) {
                float re = fft_out[2 * i];
                float im = fft_out[2 * i + 1];
                float magnitude = std::sqrt(re * re + im * im);
                float frequency = i * (44100.0f / num_samples);
                if (frequency > 50.0f && frequency < 4000.0f) {
                    int pitch = std::round(69 + 12 * std::log2(frequency / 440.0f));
                    int bin = ((pitch % 12) + 12) % 12;
                    chroma(bin) += magnitude;
                }
            }
            chroma.array() += 1e-6f;
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
            int key;
            log_viterbi_path.maxCoeff(&key);
            current_estimated_key.store(key, std::memory_order_relaxed);
        }

        if (active_features.load(std::memory_order_relaxed) & WEARINGAID_FEATURE_TEMPO) {
            // Adaptive rolling average
            energy_buf_sum -= energy_buf[energy_buf_idx];
            energy_buf[energy_buf_idx] = rms;
            energy_buf_sum += rms;
            energy_buf_idx = (energy_buf_idx + 1) % ENERGY_BUF_SIZE;
            float avg_energy = energy_buf_sum / ENERGY_BUF_SIZE;

            float frame_duration_s = static_cast<float>(num_samples) / 44100.0f;
            current_time_s += frame_duration_s;

            bool is_onset = (avg_energy > 1e-6f) &&
                            (rms > avg_energy * ONSET_THRESHOLD_RATIO) &&
                            (current_time_s - last_onset_time_s > MIN_ONSET_GAP_S);

            if (is_onset) {
                if (last_onset_time_s > 0.0f) {
                    float ioi = current_time_s - last_onset_time_s;
                    float candidate_bpm = 60.0f / ioi;
                    if (candidate_bpm >= TEMPO_BPM_MIN && candidate_bpm <= TEMPO_BPM_MAX) {
                        update_tempo_viterbi(ioi);
                        const float bpm = get_estimated_bpm();
                        tracked_bpm.store(bpm, std::memory_order_relaxed);
                        if (predicted_beat_time_s < 0.0f) {
                            predicted_beat_time_s = current_time_s + (60.0f / bpm);
                        }
                    }
                }
                last_onset_time_s = current_time_s;
            }

            if (predicted_beat_time_s > 0.0f && current_time_s >= predicted_beat_time_s) {
                haptic_pending.store(true, std::memory_order_relaxed);
                predicted_beat_time_s += 60.0f / tracked_bpm.load(std::memory_order_relaxed);
            }
        }
    }

    float get_rms() const {
        return last_frame_rms.load(std::memory_order_relaxed);
    }

    void set_genre(ProfileType type) {
        build_key_profiles(type);
        // Reset the HMM path to prevent inertia from the previous genre dragging
        log_viterbi_path = Eigen::VectorXd::Constant(24, std::log(1.0 / 24.0));
    }

    EngineOutput tick_tempo(float time_delta_seconds) {
        time_since_last_beat += time_delta_seconds;

        if (last_frame_rms.load(std::memory_order_relaxed) < SILENCE_THRESHOLD) {
            haptic_pending.store(false, std::memory_order_relaxed);
            return EngineOutput{-1, 0.0f, 0};
        }

        // exchange atomically reads haptic_pending and resets it to false in one op
        const bool trigger = haptic_pending.exchange(false, std::memory_order_relaxed);
        const uint32_t features = active_features.load(std::memory_order_relaxed);

        return EngineOutput{
            (features & WEARINGAID_FEATURE_KEY)
                ? current_estimated_key.load(std::memory_order_relaxed)
                : -1,
            tracked_bpm.load(std::memory_order_relaxed),
            (features & WEARINGAID_FEATURE_TEMPO) ? (trigger ? 1 : 0) : 0
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

float engine_get_rms(EngineState* engine) {
    if (engine && engine->instance) return engine->instance->get_rms();
    return 0.0f;
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