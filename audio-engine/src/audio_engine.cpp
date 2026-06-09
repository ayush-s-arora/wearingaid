#include "audio_engine.h"
#include "key_profiles.h"
#include "pffft.h"
#include <Eigen/Dense>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>

// Robust diagnostics: on Android these go to logcat (tag "WearingAidEngine"),
// viewable with `adb logcat -s WearingAidEngine`. On desktop they go to stderr.
// Toggle ENGINE_VERBOSE to 0 to silence the per-frame chroma/tempo dumps.
#define ENGINE_VERBOSE 1
#if defined(__ANDROID__)
  #include <android/log.h>
  #define ELOG(...) __android_log_print(ANDROID_LOG_DEBUG, "WearingAidEngine", __VA_ARGS__)
#else
  #define ELOG(...) do { std::fprintf(stderr, "[Engine] " __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#endif
#if ENGINE_VERBOSE
  #define EVLOG(...) ELOG(__VA_ARGS__)
#else
  #define EVLOG(...) ((void)0)
#endif

using namespace AudioEngine;

// 0-11 Major, 12-23 Minor. Index matches current_estimated_key.
static const char* const KEY_NAMES[24] = {
    "C",  "C#", "D",  "D#", "E",  "F",  "F#", "G",  "G#", "A",  "A#", "B",
    "Cm", "C#m","Dm", "D#m","Em", "Fm", "F#m","Gm", "G#m","Am", "A#m","Bm"
};

class HmmAudioEngine {
private:
    Eigen::MatrixXd key_profiles;          
    Eigen::MatrixXd log_transition_matrix; 
    Eigen::VectorXd log_viterbi_path;      
    
    std::atomic<uint32_t> active_features{0};

    // Actual capture sample rate, set from the audio backend (Oboe) after the stream
    // opens. Defaults to 44100 but MUST be corrected to the real rate (often 48000),
    // or every frequency→pitch and tempo calculation is systematically off.
    std::atomic<float> sample_rate{44100.0f};

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
    static constexpr float ONSET_THRESHOLD_RATIO  = 2.2f;
    // Beat-phase PLL: each onset nudges the predicted beat grid toward the actual hit
    // by this fraction of the phase error. The metronome free-runs at the tracked tempo
    // between onsets but re-locks its phase so the haptic lands ON the beat, not drifting.
    static constexpr float BEAT_PHASE_GAIN        = 0.18f;
    static constexpr int   ENERGY_BUF_SIZE        = 43;      // ~1 s of history at 1024/44100

    Eigen::VectorXd tempo_log_viterbi;
    Eigen::MatrixXd tempo_log_transition;

    float current_time_s        = 0.0f;
    float last_onset_time_s     = -1.0f;
    float predicted_beat_time_s = -1.0f;

    float energy_buf[ENERGY_BUF_SIZE] = {};
    int   energy_buf_idx              = 0;
    float energy_buf_sum              = 0.0f;

    // Autocorrelation tempo: a ring buffer of onset-strength (~6s at 1024/44100).
    // The tempo is the lag at which the envelope best correlates with itself — robust
    // to missed onsets and subdivisions, unlike per-onset interval estimation.
    static constexpr int   ENV_SIZE          = 256;
    float onset_env[ENV_SIZE] = {};
    int   env_idx             = 0;
    float prev_mag[512]       = {};  // previous frame's magnitude spectrum (EXPECTED_FRAME_SIZE/2)
    int   frames_since_tempo  = 0;
    static constexpr int   TEMPO_CALC_PERIOD = 11; // recompute tempo ~every 0.25 s
    static constexpr float TEMPO_OUTPUT_ALPHA = 0.4f; // smoothing on the reported BPM

    // Jiggle rejection: only update key detection after this many consecutive loud frames.
    int sustained_frames = 0;
    bool prev_sustained   = false;
    int silence_frames    = 0;   // consecutive frames below the silence floor
    static constexpr int   MIN_SUSTAINED_FRAMES = 4;
    static constexpr float SILENCE_THRESHOLD    = 0.030f;
    // Only a LONG silence (song ended / new context) resets the key lock. Brief gaps
    // between phrases must not wipe it — that was re-rolling the key every ~2s.
    static constexpr int   LONG_SILENCE_FRAMES  = 130; // ~3s at 1024/44100
    // Separate, higher floor for key/tempo updates: ambient noise (typing, room hum)
    // hovers at RMS 0.01–0.04 and would pollute the chroma EMA. Music sits at 0.08+.
    static constexpr float KEY_RMS_THRESHOLD    = 0.08f;
    static constexpr float ONSET_RMS_THRESHOLD  = 0.05f;

    // Chroma EMA. Key is a GLOBAL property of the song, not a per-chord readout, so the
    // average is deliberately slow (~1.8s time constant): individual chord changes wash
    // out and only the underlying tonal center survives. A warm-up integrates a few
    // frames before the first lock so the initial key is based on a stable average, not
    // one noisy frame.
    Eigen::VectorXf chroma_avg = Eigen::VectorXf::Zero(12);
    bool chroma_primed = false;
    int  chroma_warmup = 0;
    static constexpr float CHROMA_EMA_ALPHA  = 0.02f; // ~4.5s memory: report the song's KEY
                                                      // (global tonal center), not each chord
    static constexpr int   KEY_WARMUP_FRAMES = 10;  // ~0.9s of integration before first lock

    // Output hysteresis. The reported key only changes once a challenger leads the
    // committed key by KEY_SWITCH_MARGIN for KEY_SWITCH_FRAMES consecutive frames.
    // Strong values here because relative major/minor (Am vs C) are near-tied in chroma;
    // without firm hysteresis the display flickers between them every frame.
    int committed_key    = -1;   // the key actually reported; -1 = warming up / not locked
    int challenger_key   = -1;
    int challenger_count = 0;
    // Near-tied keys (relative/parallel/dominant share most scale notes) score within ~1%
    // of each other, so MARGIN alone can't tell a real change from noise — only TIME can.
    // Require a challenger to hold a small lead for ~1.1s of consecutive frames: that rejects
    // sub-second flicker between near-ties while still catching genuine multi-second changes.
    static constexpr int    KEY_SWITCH_FRAMES = 12;
    static constexpr double KEY_SWITCH_MARGIN = 0.005;

    // Cached PFFFT resources; allocated once, reused every callback.
    static constexpr int EXPECTED_FRAME_SIZE = 1024;
    PFFFT_Setup* fft_setup = nullptr;
    float*       fft_in    = nullptr;
    float*       fft_out   = nullptr;
    float*       fft_work  = nullptr;

    // 4096-point FFT for chroma: 10.8 Hz/bin vs 43 Hz/bin for 1024.
    // At 43 Hz/bin, G2 (98 Hz) and F2 (87 Hz) land in the same bin and both read as F.
    // At 10.8 Hz/bin they separate cleanly. Runs every 4 callbacks (~93ms).
    static constexpr int CHROMA_FFT_SIZE = 4096;
    PFFFT_Setup* chroma_fft_setup = nullptr;
    float*       chroma_fft_in    = nullptr; // doubles as accumulation ring
    float*       chroma_fft_out   = nullptr;
    float*       chroma_fft_work  = nullptr;
    int          chroma_acc_fill  = 0;

    void build_key_profiles(ProfileType type) {
        const double* maj_ptr = nullptr;
        const double* min_ptr = nullptr;

        switch (type) {
            case ProfileType::TEMPERLEY:
                maj_ptr = Profiles::TEMPERLEY_MAJ.data();
                min_ptr = Profiles::TEMPERLEY_MIN.data();
                ELOG("profile -> TEMPERLEY (Classical)");
                break;
            case ProfileType::EDMA:
                maj_ptr = Profiles::EDMA_MAJ.data();
                min_ptr = Profiles::EDMA_MIN.data();
                ELOG("profile -> EDMA (Electronic)");
                break;
            case ProfileType::WEI_CHAI:
                maj_ptr = Profiles::WEI_CHAI_MAJ.data();
                min_ptr = Profiles::WEI_CHAI_MIN.data();
                ELOG("profile -> WEI CHAI (Acoustic/Folk)");
                break;
            case ProfileType::TONIC_TRIAD:
                maj_ptr = Profiles::TONIC_TRIAD_MAJ.data();
                min_ptr = Profiles::TONIC_TRIAD_MIN.data();
                ELOG("profile -> TONIC TRIAD (Minimalist)");
                break;
            case ProfileType::SHAATH:
            default:
                maj_ptr = Profiles::SHAATH_MAJ.data();
                min_ptr = Profiles::SHAATH_MIN.data();
                ELOG("profile -> SHAATH (Pop/Rock/Jazz)");
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

    // Estimate tempo by autocorrelating the onset-strength envelope. The lag (in frames)
    // with the strongest self-similarity is the beat period. A perceptual weight biased
    // toward ~120 BPM resolves the octave (half/double) ambiguity toward the metrical
    // level a human would tap. Far steadier than per-onset intervals.
    void estimate_tempo_autocorr(float frame_dur_s) {
        // Copy the ring buffer into time order and remove the mean (DC) so the
        // correlation reflects rhythmic structure, not overall loudness.
        float env[ENV_SIZE];
        float mean = 0.0f;
        for (int i = 0; i < ENV_SIZE; ++i) { env[i] = onset_env[(env_idx + i) % ENV_SIZE]; mean += env[i]; }
        mean /= ENV_SIZE;
        for (int i = 0; i < ENV_SIZE; ++i) env[i] -= mean;

        int lag_min = static_cast<int>(std::floor((60.0f / TEMPO_BPM_MAX) / frame_dur_s)); // fastest
        int lag_max = static_cast<int>(std::ceil ((60.0f / TEMPO_BPM_MIN) / frame_dur_s)); // slowest
        if (lag_max >= ENV_SIZE) lag_max = ENV_SIZE - 1;

        const float prev_bpm = tracked_bpm.load(std::memory_order_relaxed);
        float best_score = 0.0f, best_corr = 0.0f;
        int   best_lag   = 0;
        for (int lag = lag_min; lag <= lag_max; ++lag) {
            float corr = 0.0f;
            for (int i = lag; i < ENV_SIZE; ++i) corr += env[i] * env[i - lag];
            corr /= (ENV_SIZE - lag);                       // unbias: longer lags have fewer terms
            float bpm = 60.0f / (lag * frame_dur_s);
            // Perceptual preference centered ~120 BPM. Continuity term is intentionally GENTLE
            // (wide): it only resists big octave-scale jumps (e.g. a warm-up 144 → true 73)
            // without forcing the estimate to slide and rail at the slowest lag — an aggressive
            // continuity caused exactly that low-rail lock on sparse ballads.
            float lw  = std::log2(bpm / 120.0f) / 0.9f;
            float cw  = std::log2(bpm / prev_bpm) / 1.0f;
            float score = corr * std::exp(-0.5f * lw * lw) * std::exp(-0.5f * cw * cw);
            if (score > best_score) { best_score = score; best_corr = corr; best_lag = lag; }
        }

        // Only adopt a new estimate when there's real periodicity; otherwise hold the
        // last tempo (keeps the haptic steady through ambiguous passages).
        if (best_lag > 0 && best_corr > 0.0f) {
            float bpm  = 60.0f / (best_lag * frame_dur_s);
            float smoothed = (1.0f - TEMPO_OUTPUT_ALPHA) * prev_bpm + TEMPO_OUTPUT_ALPHA * bpm;
            tracked_bpm.store(smoothed, std::memory_order_relaxed);
            EVLOG("TEMPO autocorr lag=%d rawBPM=%.0f corr=%.3f -> tracked=%.0f", best_lag, bpm, best_corr, smoothed);
        }
    }

    void initialize_matrices() {
        key_profiles = Eigen::MatrixXd::Zero(24, 12);
        log_transition_matrix = Eigen::MatrixXd::Zero(24, 24);
        log_viterbi_path = Eigen::VectorXd::Zero(24);

        // Default to the contemporary band profile
        build_key_profiles(ProfileType::SHAATH);
        initialize_tempo();

        // Inertial transition: 70% hold probability. Lower than the original 85%
        // so the HMM can respond to real key changes without locking on noise.
        for (int i = 0; i < 24; ++i) {
            for (int j = 0; j < 24; ++j) {
                log_transition_matrix(i, j) = (i == j) ? std::log(0.70) : std::log(0.30 / 23.0);
            }
        }
    }

public:
    HmmAudioEngine(uint32_t features) : active_features(features) {
        fft_setup = pffft_new_setup(EXPECTED_FRAME_SIZE, PFFFT_REAL);
        fft_in    = (float*)pffft_aligned_malloc(EXPECTED_FRAME_SIZE * sizeof(float));
        fft_out   = (float*)pffft_aligned_malloc(EXPECTED_FRAME_SIZE * sizeof(float));
        fft_work  = (float*)pffft_aligned_malloc(EXPECTED_FRAME_SIZE * sizeof(float));
        chroma_fft_setup = pffft_new_setup(CHROMA_FFT_SIZE, PFFFT_REAL);
        chroma_fft_in    = (float*)pffft_aligned_malloc(CHROMA_FFT_SIZE * sizeof(float));
        chroma_fft_out   = (float*)pffft_aligned_malloc(CHROMA_FFT_SIZE * sizeof(float));
        chroma_fft_work  = (float*)pffft_aligned_malloc(CHROMA_FFT_SIZE * sizeof(float));
        std::fill(chroma_fft_in, chroma_fft_in + CHROMA_FFT_SIZE, 0.0f);
        initialize_matrices();
        ELOG("created | features=0x%x | chromaFFT=%d (%.1f Hz/bin @44100) | silenceThr=%.3f keyThr=%.3f onsetThr=%.3f",
             features, CHROMA_FFT_SIZE, 44100.0f / CHROMA_FFT_SIZE,
             SILENCE_THRESHOLD, KEY_RMS_THRESHOLD, ONSET_RMS_THRESHOLD);
    }

    ~HmmAudioEngine() {
        pffft_destroy_setup(fft_setup);
        pffft_aligned_free(fft_in);
        pffft_aligned_free(fft_out);
        pffft_aligned_free(fft_work);
        pffft_destroy_setup(chroma_fft_setup);
        pffft_aligned_free(chroma_fft_in);
        pffft_aligned_free(chroma_fft_out);
        pffft_aligned_free(chroma_fft_work);
    }

    void set_features(uint32_t features) {
        active_features.store(features, std::memory_order_relaxed);
        ELOG("set_features -> 0x%x (key=%d tempo=%d)", features,
             (features & WEARINGAID_FEATURE_KEY) ? 1 : 0,
             (features & WEARINGAID_FEATURE_TEMPO) ? 1 : 0);
    }

    void set_sample_rate(int sr) {
        if (sr > 0) {
            sample_rate.store(static_cast<float>(sr), std::memory_order_relaxed);
            ELOG("set_sample_rate -> %d Hz (chroma bin = %.2f Hz)", sr, sr / (float)CHROMA_FFT_SIZE);
        }
    }

    void process_audio(const float* pcm_data, int num_samples) {
        if (num_samples != EXPECTED_FRAME_SIZE) return;

        const float sr = sample_rate.load(std::memory_order_relaxed);

        static constexpr float GATE_GAIN = 20.0f;
        float rms_sq = 0.0f;
        for (int i = 0; i < num_samples; ++i) {
            float g = std::tanh(pcm_data[i] * GATE_GAIN);
            rms_sq += g * g;
        }
        // Accumulate raw PCM into the 4096-sample chroma buffer.
        // Gain is applied just before the FFT so it doesn't distort the accumulation.
        std::memcpy(chroma_fft_in + chroma_acc_fill, pcm_data, num_samples * sizeof(float));
        chroma_acc_fill += num_samples;

        // RMS on the amplified signal so the sustain gate sees the boosted level
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

        // Track silence duration. A brief gap between phrases preserves the key lock;
        // only a sustained ~3s silence (song over / put down) wipes it so the next
        // piece starts fresh.
        if (rms < SILENCE_THRESHOLD) {
            if (++silence_frames == LONG_SILENCE_FRAMES) {
                chroma_avg = Eigen::VectorXf::Zero(12);
                chroma_primed = false;
                chroma_warmup = 0;
                committed_key = -1;
                challenger_key = -1; challenger_count = 0;
                chroma_acc_fill = 0;
                current_estimated_key.store(-1, std::memory_order_relaxed);
                // Clear the onset envelope so the next piece's tempo isn't correlated
                // against stale rhythm from the previous one.
                std::fill(onset_env, onset_env + ENV_SIZE, 0.0f);
                predicted_beat_time_s = -1.0f;
                last_onset_time_s = -1.0f;
                EVLOG("--- long silence (~3s), key + tempo reset ---");
            }
        } else {
            silence_frames = 0;
        }

        const bool now_sustained = (sustained_frames >= MIN_SUSTAINED_FRAMES);
        prev_sustained = now_sustained;

        // Key detection fires every CHROMA_FFT_SIZE samples (~93ms) when sustained.
        // The 4096-point FFT resolves bass semitones: G2 (98 Hz) and F2 (87 Hz) are
        // 11 Hz apart — only separable at 10.8 Hz/bin, not at 43 Hz/bin (1024-pt FFT).
        if (chroma_acc_fill >= CHROMA_FFT_SIZE) {
            if ((active_features.load(std::memory_order_relaxed) & WEARINGAID_FEATURE_KEY) &&
                now_sustained && rms >= KEY_RMS_THRESHOLD) {
                // Hann window, linear signal (NO nonlinear gain). The previous tanh(·×4)
                // soft-clipped the waveform and injected spurious odd harmonics (a note's
                // 5th and 3rd), smearing energy across pitch classes; the rectangular FFT
                // added spectral leakage on top. Both left every key scoring within ~0.5%
                // of the others. Windowing + linear input preserves the true spectral shape.
                for (int i = 0; i < CHROMA_FFT_SIZE; ++i) {
                    float w = 0.5f * (1.0f - std::cos(6.2831853072f * i / (CHROMA_FFT_SIZE - 1)));
                    chroma_fft_in[i] *= w;
                }
                pffft_transform_ordered(chroma_fft_setup, chroma_fft_in, chroma_fft_out, chroma_fft_work, PFFFT_FORWARD);

                // Sum spectral ENERGY (magnitude²) per pitch class. Energy weighting
                // emphasizes tonal peaks over the broadband noise floor this mic produces.
                // No per-bin-count division: that averages the few strong harmonic peaks
                // down into the many empty bins, flattening the chroma until argmax always
                // ties to index 0 (C Major). Binning is symmetric across pitch classes, so
                // summing energy introduces no per-key bias.
                Eigen::VectorXf chroma = Eigen::VectorXf::Zero(12);
                for (int i = 1; i < CHROMA_FFT_SIZE / 2; ++i) {
                    float re = chroma_fft_out[2 * i];
                    float im = chroma_fft_out[2 * i + 1];
                    float energy = re * re + im * im;
                    float frequency = i * (sr / CHROMA_FFT_SIZE);
                    // 100–2000 Hz: the fundamental band. The low floor MUST include the
                    // bass register (C2≈65, C3≈130, G2≈98, A2≈110) where chord ROOTS live —
                    // a 200 Hz floor deletes the tonic and leaves only upper harmonics, which
                    // for a C-major piece are dominated by E and E's 5th-harmonic G#, pulling
                    // the estimate away from the true key. The 2000 Hz ceiling trims the
                    // harmonic-rich top end that smears pitch classes together.
                    if (frequency > 100.0f && frequency < 2000.0f) {
                        int pitch = std::round(69 + 12 * std::log2(frequency / 440.0f));
                        int bin = ((pitch % 12) + 12) % 12;
                        chroma(bin) += energy;
                    }
                }
                chroma.array() += 1e-9f;
                chroma = chroma.array() / chroma.sum();

                // Tonality gate: skip when the chromagram is nearly uniform (noise/percussion).
                const float chroma_peak = chroma.maxCoeff();
                if (chroma_peak >= 2.0f / 12.0f) {
                    if (!chroma_primed) {
                        chroma_avg = chroma; // first real frame: skip blend with zeros
                        chroma_primed = true;
                        chroma_warmup = 1;
                    } else {
                        chroma_avg = (1.0f - CHROMA_EMA_ALPHA) * chroma_avg + CHROMA_EMA_ALPHA * chroma;
                        if (chroma_warmup < KEY_WARMUP_FRAMES) chroma_warmup++;
                    }
                    Eigen::VectorXd scores = key_profiles * chroma_avg.cast<double>();
                    int best; double best_score = scores.maxCoeff(&best);
                    double second_score = -1e9; int second = -1;
                    for (int k = 0; k < 24; ++k)
                        if (k != best && scores(k) > second_score) { second_score = scores(k); second = k; }

                    if (chroma_warmup < KEY_WARMUP_FRAMES) {
                        // Still integrating — report nothing yet (watch shows "Listening").
                    } else if (committed_key < 0) {
                        // First lock: take the stable warmed-up average's winner.
                        committed_key = best;
                        challenger_key = -1; challenger_count = 0;
                    } else if (best == committed_key) {
                        challenger_key = -1; challenger_count = 0;
                    } else {
                        const bool leads = best_score > scores(committed_key) * (1.0 + KEY_SWITCH_MARGIN);
                        if (leads && best == challenger_key) {
                            if (++challenger_count >= KEY_SWITCH_FRAMES) {
                                committed_key = best;
                                challenger_key = -1; challenger_count = 0;
                            }
                        } else if (leads) {
                            challenger_key = best; challenger_count = 1;
                        } else {
                            challenger_key = -1; challenger_count = 0;
                        }
                    }
                    current_estimated_key.store(committed_key, std::memory_order_relaxed);

                    EVLOG("KEY raw %-3s (%.3f) | committed %-3s | 2nd %-3s (%.3f) margin %.3f peak %.2f warm %d | chroma C%.2f C#%.2f D%.2f D#%.2f E%.2f F%.2f F#%.2f G%.2f G#%.2f A%.2f A#%.2f B%.2f",
                          KEY_NAMES[best], best_score,
                          committed_key >= 0 ? KEY_NAMES[committed_key] : "--",
                          second >= 0 ? KEY_NAMES[second] : "-", second_score,
                          best_score - second_score, chroma_peak, chroma_warmup,
                          chroma_avg(0), chroma_avg(1), chroma_avg(2), chroma_avg(3),
                          chroma_avg(4), chroma_avg(5), chroma_avg(6), chroma_avg(7),
                          chroma_avg(8), chroma_avg(9), chroma_avg(10), chroma_avg(11));
                } else {
                    EVLOG("KEY skip (flat chroma, peak %.2f < %.2f)", chroma_peak, 2.0f / 12.0f);
                }
            }
            chroma_acc_fill = 0;
        }

        if (active_features.load(std::memory_order_relaxed) & WEARINGAID_FEATURE_TEMPO) {
            const float frame_duration_s = static_cast<float>(num_samples) / sr;
            current_time_s += frame_duration_s;

            // 1. Onset-strength envelope via SPECTRAL FLUX: sum of positive change in
            //    each frequency bin vs the previous frame. Spikes on note attacks (where
            //    beats live), so the envelope has strong beat-level periodicity — unlike
            //    gross RMS change, which only tracks slow swells and locked onto the
            //    wrong (sub-beat) period. Reuses the otherwise-idle 1024-pt FFT.
            for (int i = 0; i < num_samples; ++i) fft_in[i] = pcm_data[i];
            pffft_transform_ordered(fft_setup, fft_in, fft_out, fft_work, PFFFT_FORWARD);
            float flux = 0.0f;
            for (int k = 1; k < num_samples / 2; ++k) {
                float re = fft_out[2 * k], im = fft_out[2 * k + 1];
                float mag = std::sqrt(re * re + im * im);
                float d = mag - prev_mag[k];
                if (d > 0.0f) flux += d;
                prev_mag[k] = mag;
            }
            onset_env[env_idx] = flux;
            env_idx = (env_idx + 1) % ENV_SIZE;

            // 2. Re-estimate the tempo (period) ~4x/sec from the whole envelope.
            if (++frames_since_tempo >= TEMPO_CALC_PERIOD) {
                frames_since_tempo = 0;
                estimate_tempo_autocorr(frame_duration_s);
            }

            // 3. Onset events only drive the beat-phase PLL now (period comes from the
            //    autocorrelation, which is far steadier than per-onset intervals).
            energy_buf_sum -= energy_buf[energy_buf_idx];
            energy_buf[energy_buf_idx] = rms;
            energy_buf_sum += rms;
            energy_buf_idx = (energy_buf_idx + 1) % ENERGY_BUF_SIZE;
            float avg_energy = energy_buf_sum / ENERGY_BUF_SIZE;

            bool is_onset = (avg_energy > 1e-6f) &&
                            (rms >= ONSET_RMS_THRESHOLD) &&
                            (rms > avg_energy * ONSET_THRESHOLD_RATIO) &&
                            (current_time_s - last_onset_time_s > MIN_ONSET_GAP_S);

            if (is_onset) {
                const float beat_period = 60.0f / tracked_bpm.load(std::memory_order_relaxed);
                if (predicted_beat_time_s < 0.0f) {
                    predicted_beat_time_s = current_time_s + beat_period;
                } else {
                    float err = current_time_s - predicted_beat_time_s;
                    err -= std::round(err / beat_period) * beat_period; // wrap to nearest beat
                    // Deadzone: ignore hits near ±half-beat (clear syncopations) so eighth-note
                    // off-beats don't drag the phase; on-beat hits cluster near err≈0 and lock.
                    if (std::fabs(err) < 0.35f * beat_period) {
                        predicted_beat_time_s += BEAT_PHASE_GAIN * err;
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
        chroma_avg = Eigen::VectorXf::Zero(12);
        chroma_primed = false;
        chroma_warmup = 0;
        committed_key = -1;
        challenger_key = -1;
        challenger_count = 0;
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

void engine_set_sample_rate(EngineState* engine, int sample_rate) {
    if (engine && engine->instance) engine->instance->set_sample_rate(sample_rate);
}

void engine_destroy(EngineState* engine) {
    if (engine) { 
        delete engine->instance; 
        delete engine; 
    }
}