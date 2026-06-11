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

#define ENGINE_BUILD_TAG "v-6-11-1125"

// 0-11 Major, 12-23 Minor. Index matches current_estimated_key.
static const char* const KEY_NAMES[24] = {
    "C",  "C#", "D",  "D#", "E",  "F",  "F#", "G",  "G#", "A",  "A#", "B",
    "Cm", "C#m","Dm", "D#m","Em", "Fm", "F#m","Gm", "G#m","Am", "A#m","Bm"
};

class HmmAudioEngine {
private:
    Eigen::MatrixXd key_profiles;       // 24x12, L1-normalized genre profiles
    Eigen::MatrixXd key_profiles_norm;  // 24x12, rows mean-centered + L2-normalized (for Pearson)
    Eigen::MatrixXd key_transition;     // 24x24 row-stochastic HMM transition matrix
    Eigen::VectorXd key_log_post;       // 24, running log-posterior (forward filter)

    std::atomic<uint32_t> active_features{0};

    // Actual capture sample rate, set from the audio backend (Oboe) after the stream
    // opens. Defaults to 44100 but MUST be corrected to the real rate (often 48000),
    // or every frequency->pitch and tempo calculation is systematically off.
    std::atomic<float> sample_rate{44100.0f};

    // Cross-thread values: written by process_audio (Oboe callback thread),
    // read by tick_tempo (Kotlin coroutine thread). std::atomic ensures no torn reads.
    std::atomic<int>   current_estimated_key{-1};
    std::atomic<float> tracked_bpm{120.0f};
    std::atomic<bool>  haptic_pending{false};
    std::atomic<float> last_frame_rms{0.0f};
    std::atomic<bool>  output_muted{true}; // true after ~0.5s of silence (see OUTPUT_MUTE_FRAMES)

    float time_since_last_beat = 0.0f;

    static constexpr int   TEMPO_BPM_MIN          = 60;
    static constexpr int   TEMPO_BPM_MAX          = 180;
    static constexpr float MIN_ONSET_GAP_S        = 0.25f;
    static constexpr float ONSET_THRESHOLD_RATIO  = 2.2f;
    // Beat-phase PLL: each onset nudges the predicted beat grid toward the actual hit
    // by this fraction of the phase error. The metronome free-runs at the tracked tempo
    // between onsets but re-locks its phase so the haptic lands ON the beat, not drifting.
    static constexpr float BEAT_PHASE_GAIN        = 0.18f;
    static constexpr int   ENERGY_BUF_SIZE        = 43;      // ~1 s of history at 1024/44100

    float current_time_s        = 0.0f;
    float last_onset_time_s     = -1.0f;
    float predicted_beat_time_s = -1.0f;

    float energy_buf[ENERGY_BUF_SIZE] = {};
    int   energy_buf_idx              = 0;
    float energy_buf_sum              = 0.0f;

    // Autocorrelation tempo: a ring buffer of onset-strength (~12s at 1024/44100).
    // The tempo is the lag at which the envelope best correlates with itself -- robust
    // to missed onsets and subdivisions, unlike per-onset interval estimation.
    // 512 frames so slow songs (72 BPM = one beat per 36 frames) still contribute
    // a dozen-plus beat periods to the correlation, not just six noisy ones.
    static constexpr int   ENV_SIZE          = 512;
    float onset_env[ENV_SIZE] = {};
    int   env_idx             = 0;
    // Validity mask for the envelope: 0 = motor-contaminated or ambient-only frame.
    // The correlation is computed over VALID PAIRS only, normalized per-lag by the
    // pair count (masked autocorrelation). This is the only motor defense that holds:
    // zero-blanking imprinted the blanking period (1.5x lock), mean-imputation gated
    // the correlation periodically (lag pinned at exactly 14.00), and spectral
    // notching missed the motor's BROADBAND attack click (83 BPM self-lock at lag
    // 31). Masking with per-lag normalization makes the mask's own periodicity
    // cancel out of the num/den ratio.
    uint8_t env_mask[ENV_SIZE] = {};
    // Consecutive low-confidence tempo estimates. After ~4s without real
    // periodicity the haptic stops firing instead of metronoming a stale guess.
    int   low_conf_streak     = 0;
    float prev_mag[512]       = {};  // previous frame's magnitude spectrum (EXPECTED_FRAME_SIZE/2)
    float flux_mean           = 0.0f; // running mean of spectral flux
    int   frames_since_tempo  = 0;
    static constexpr int   TEMPO_CALC_PERIOD = 11; // recompute tempo ~every 0.25 s
    static constexpr float TEMPO_OUTPUT_ALPHA = 0.4f; // smoothing on the reported BPM
    // Raw (unsmoothed) autocorrelation winner -- used ONLY for the continuity prior
    // so the EMA display smoothing doesn't feed back into the scoring function.
    // The old design fed tracked_bpm (smoothed) into the continuity prior: when the
    // raw estimate flipped between octave levels (72<->144), the smoothed value walked
    // through intermediate tempos (~100, ~115), and the prior saw those intermediates
    // as "current" -- neither octave level was close, so the winner oscillated wildly.
    float raw_bpm = 120.0f;

    // Slow loudness average (~0.5s). Music on this mic sits at RMS 0.08+; ambient
    // room sound (speech cadence, appliances) sits lower but is periodic enough to
    // score tempo confidence 0.4-0.7 -- a 150 BPM lock acquired BEFORE the music
    // even started, which the continuity prior then dragged into the song's first
    // half-minute. The tempo pipeline is gated on this the same way the key is.
    float rms_ema = 0.0f;

    // Jiggle rejection: only update key detection after this many consecutive loud frames.
    int sustained_frames = 0;
    bool prev_sustained   = false;
    int silence_frames    = 0;   // consecutive frames below the silence floor
    static constexpr int   MIN_SUSTAINED_FRAMES = 4;
    static constexpr float SILENCE_THRESHOLD    = 0.030f;
    // Only a LONG silence (song ended / new context) resets the key lock. Brief gaps
    // between phrases must not wipe it -- that was re-rolling the key every ~2s.
    static constexpr int   LONG_SILENCE_FRAMES  = 130; // ~3s at 1024/44100
    // The OUTPUT (key/BPM shown on the watch) mutes after ~0.5s of silence rather than
    // on a single quiet 23ms frame: gating on instantaneous RMS made the display and
    // BPM readout flicker to "Listening"/0 in the gaps between notes mid-song.
    static constexpr int   OUTPUT_MUTE_FRAMES   = 22;  // ~0.5s at 1024/44100

    // Haptic self-noise rejection for the KEY path only: chroma windows containing
    // the buzz are skipped (the key HMM has no autocorrelation to bias, so window
    // skipping is safe there -- unlike the tempo envelope, see flux_mean note).
    // The motor buzzes at ~190 Hz (the F#3/G3 boundary) exactly ON each predicted
    // beat. tick_tempo sets haptic_just_fired when it hands the watch a vibration;
    // the audio thread then marks the next frames contaminated.
    // The window must absorb INPUT LATENCY too: with PerformanceMode::None, the
    // 48k->44.1k resampler and 882-frame bursts, the buzz reaches the callback well
    // after the trigger instant -- a 140ms window measurably missed it on-device.
    static constexpr int   HAPTIC_SUPPRESS_FRAMES = 12; // ~280ms: latency + 16-52ms buzz + ring-down
    std::atomic<bool> haptic_just_fired{false};
    int  haptic_suppress_left = 0;   // audio-thread only
    bool chroma_contaminated  = false; // current 4096 chroma window contains motor frames
    // Floor for key updates. Was 0.08 to keep ambient noise out of the chroma EMA,
    // but peak picking + the tonality gate now reject noise structurally, and 0.08
    // blanked the key entirely during quiet playback (a whole run produced zero KEY
    // frames for 24s because the music sat just under it).
    static constexpr float KEY_RMS_THRESHOLD    = 0.05f;
    static constexpr float ONSET_RMS_THRESHOLD  = 0.05f;

    // Chroma EMA: a short denoising blend only (~0.6s). The long-term "key is a global
    // property" integration now lives in the HMM forward filter, not here -- a 4.5s EMA
    // on top of the HMM just delayed every response without adding stability.
    Eigen::VectorXf chroma_avg = Eigen::VectorXf::Zero(12);
    bool chroma_primed = false;
    int  chroma_warmup = 0;
    static constexpr float CHROMA_EMA_ALPHA  = 0.15f;
    static constexpr int   KEY_WARMUP_FRAMES = 10;  // ~0.9s of integration before first lock

    // HMM forward filter over the 24 keys. Each ~93ms chroma frame is one VOTE (a G
    // chord frame votes G-ish, a C chord frame votes C-ish); the filter accumulates
    // votes under a strong stay-put prior, so the key that explains ALL the chords in
    // the progression wins (C-G-Am-F -> C major), instead of the display chasing
    // whichever chord is currently sounding -- that chase was the G/Em/D wandering.
    static constexpr double KEY_SELF_PROB      = 0.99; // ~9s expected dwell per key
    // Softmax temperature on the Pearson r emissions. This sets the INTEGRATION TIME:
    // at 12.0 a single 2s chord (~21 frames x ~0.1 r-advantage) injected >25 nats and
    // ran the posterior wall-to-wall -- the HMM followed the chord progression
    // (Dm -> F -> G on-device) instead of the key. At 3.5 a transient chord moves the
    // posterior ~7 nats, under the clamp, while a true key change (sustained ~0.05
    // average advantage) still flips the argmax in ~5s.
    static constexpr double KEY_EMISSION_BETA  = 3.5;
    // Floor on how far a key's log-posterior may fall below the leader. Bounds how
    // long a genuine key change needs to overturn the incumbent -- without it the
    // posterior saturates and never switches. 11 nats ~ 8-10s of sustained contrary
    // evidence: an E-dominant bridge section eroded a correct C lock (gap 7 -> 0) in
    // ~5s at the previous 8.0 -- chord-region sections must not steal the display.
    static constexpr double KEY_LOG_POST_CLAMP = 11.0;

    // Display hysteresis on top of the HMM (the posterior argmax can flicker briefly
    // right after warm-up while evidence is still thin).
    int committed_key    = -1;   // the key actually reported; -1 = warming up / not locked
    int challenger_key   = -1;
    int challenger_count = 0;
    // The display only follows DECISIVE posterior flips: the challenger must lead
    // the committed key by a real margin, continuously, for ~1.5s. A hair-thin
    // argmax flip held for 0.5s was enough before -- that was the now-and-then
    // Em/G flicker on a C major song. A genuine modulation sustains its lead for
    // many seconds, so the added latency is invisible; the flicker is not.
    static constexpr int    KEY_SWITCH_FRAMES      = 16;  // ~1.5s of consecutive decisive lead
    static constexpr double KEY_SWITCH_MARGIN_NATS = 2.5; // challenger's lead over committed
    // Closely related keys (relative, parallel, fifth, mediant -- C<->Em is THE
    // flicker pair) must clear a higher bar: through a small speaker + watch mic
    // the bass roots that separate C from Em barely arrive, so the pair stays
    // near-tied live even when offline audio is decisive. A genuine modulation
    // to a related key sustains its lead for tens of seconds and still passes.
    static constexpr double KEY_SWITCH_MARGIN_RELATED = 4.5;
    // The FIRST lock additionally waits for the posterior to separate. Committing on
    // a near-tie (gap 0.08 on-device, polluted by early motor-buzz windows) published
    // a junk key that took ~14s of hysteresis walking (Bm->Am->C->G->Em) to escape.
    // Better to show "Listening" ~2s longer and start on the right key.
    // 3.0 because a melody-section warm-up twice locked the relative minor at ~2.2.
    static constexpr double KEY_FIRST_LOCK_GAP = 3.0; // nats between best and runner-up

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
    float chroma_mag_buf[CHROMA_FFT_SIZE / 2] = {}; // scratch magnitude spectrum for peak picking
    int   peaks_log_counter = 0;                    // rate-limits the PEAKS debug dump

    // Stationary-tone ("drone") rejection: a per-bin LEAKY MINIMUM of the magnitude
    // spectrum. A constant tone (vibration motor ~190 Hz, mains hum, a fan) keeps its
    // bin's minimum high, so subtracting the tracked floor erases it; real notes come
    // and go faster than the floor can rise (~5%/93ms => a note must hold ~9s before
    // it starts being treated as a drone), so music passes through. On-device, a tone
    // pinned at 189-194 Hz -- squarely BETWEEN F#3 (185) and G3 (196), i.e. not a note --
    // dominated nearly every window and drove the key estimate; this removes it and
    // anything like it without needing to know the source.
    float drone_mag[CHROMA_FFT_SIZE / 2] = {};
    int   starved_windows = 0; // chroma windows lost to haptic suppression (diagnostic)
    bool  drone_primed   = false;
    float drone_freq_hz  = 0.0f;  // dominant stationary tone; audio thread only
    bool  drone_active   = false; // true when the drone rivals the music in level
    static constexpr float DRONE_RISE     = 1.05f; // leaky-min rise per chroma frame
    static constexpr float DRONE_SUBTRACT = 2.0f;  // overshoot so the tone's wobble dies too
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

        // Precompute mean-centered, L2-normalized rows so the emission score is the
        // PEARSON correlation with the observed chroma. Unlike the raw dot product,
        // Pearson penalizes energy where the profile expects none -- which is exactly
        // the F vs F# distinction that separates C major from G major.
        key_profiles_norm = Eigen::MatrixXd::Zero(24, 12);
        for (int k = 0; k < 24; ++k) {
            Eigen::RowVectorXd row = key_profiles.row(k);
            row.array() -= row.mean();
            double n = row.norm();
            if (n > 1e-12) key_profiles_norm.row(k) = row / n;
        }
    }

    // Row-stochastic transition matrix over the 24 keys. Keys are predictable:
    // modulations overwhelmingly go to circle-of-fifths neighbors, the relative
    // major/minor, or the parallel key, so those get most of the off-diagonal mass.
    void build_key_transition() {
        key_transition = Eigen::MatrixXd::Zero(24, 24);
        for (int i = 0; i < 24; ++i) {
            const int  tonic_i = i % 12;
            const bool major_i = i < 12;
            double weights[24] = {};
            double weight_sum = 0.0;
            for (int j = 0; j < 24; ++j) {
                if (j == i) continue;
                const int  tonic_j = j % 12;
                const bool major_j = j < 12;
                double w = 1.0;
                if (major_i != major_j) {
                    const bool relative = major_i ? (tonic_j == (tonic_i + 9) % 12)
                                                  : (tonic_j == (tonic_i + 3) % 12);
                    if (relative)               w = 6.0;  // C <-> Am
                    else if (tonic_i == tonic_j) w = 4.0; // C <-> Cm
                } else {
                    const int d = (tonic_j - tonic_i + 12) % 12;
                    if (d == 5 || d == 7)       w = 5.0;  // C <-> F / C <-> G
                }
                weights[j] = w;
                weight_sum += w;
            }
            for (int j = 0; j < 24; ++j) {
                key_transition(i, j) = (j == i)
                    ? KEY_SELF_PROB
                    : (1.0 - KEY_SELF_PROB) * weights[j] / weight_sum;
            }
        }
    }

    void reset_key_posterior() {
        key_log_post = Eigen::VectorXd::Constant(24, -std::log(24.0));
    }

    // True for the key pairs that share most of their scale tones and therefore
    // stay near-tied in noisy conditions: relative (C<->Am), parallel (C<->Cm),
    // fifth-related (C<->G/F), and mediant (C<->Em / Eb<->Cm).
    static bool keys_closely_related(int a, int b) {
        const int  ta = a % 12, tb = b % 12;
        const bool major_a = a < 12, major_b = b < 12;
        if (major_a == major_b) {
            const int d = (tb - ta + 12) % 12;
            return d == 5 || d == 7;                       // fifth circle neighbors
        }
        const int t_maj = major_a ? ta : tb;
        const int t_min = major_a ? tb : ta;
        return t_min == t_maj ||                           // parallel
               t_min == (t_maj + 9) % 12 ||                // relative
               t_min == (t_maj + 4) % 12;                  // mediant (C<->Em)
    }

    // Estimate tempo by autocorrelating the onset-strength envelope. The lag (in frames)
    // with the strongest self-similarity is the beat period. A perceptual weight biased
    // toward ~120 BPM resolves the octave (half/double) ambiguity toward the metrical
    // level a human would tap. Far steadier than per-onset intervals.
    void estimate_tempo_autocorr(float frame_dur_s) {
        // Copy the ring buffer into time order and remove the mean (DC) so the
        // correlation reflects rhythmic structure, not overall loudness.
        // We use env_mask to compute mean and correlations only over valid frames.
        float env[ENV_SIZE];
        uint8_t mask[ENV_SIZE];
        float mean = 0.0f;
        int valid_count = 0;
        for (int i = 0; i < ENV_SIZE; ++i) {
            env[i] = onset_env[(env_idx + i) % ENV_SIZE];
            mask[i] = env_mask[(env_idx + i) % ENV_SIZE];
            if (mask[i]) {
                mean += env[i];
                valid_count++;
            }
        }
        if (valid_count > 0) mean /= valid_count;
        for (int i = 0; i < ENV_SIZE; ++i) env[i] -= mean;

        int lag_min = static_cast<int>(std::floor((60.0f / TEMPO_BPM_MAX) / frame_dur_s)); // fastest
        int lag_max = static_cast<int>(std::ceil ((60.0f / TEMPO_BPM_MIN) / frame_dur_s)); // slowest
        if (lag_min < 1) lag_min = 1;
        if (lag_max >= ENV_SIZE) lag_max = ENV_SIZE - 1;

        // Unbiased autocorrelation for every lag up to 3x the slowest beat period, so
        // each tempo candidate can also be scored with its harmonics below.
        const int corr_lags = std::min(3 * lag_max, ENV_SIZE - 1);
        float corr[ENV_SIZE] = {};
        for (int lag = 1; lag <= corr_lags; ++lag) {
            float c = 0.0f;
            int pairs = 0;
            for (int i = lag; i < ENV_SIZE; ++i) {
                if (mask[i] && mask[i - lag]) {
                    c += env[i] * env[i - lag];
                    pairs++;
                }
            }
            if (pairs > 0) corr[lag] = c / pairs;
        }

        // Zero-lag autocorrelation = envelope variance; the reference for confidence.
        float corr0 = 0.0f;
        int pairs0 = 0;
        for (int i = 0; i < ENV_SIZE; ++i) {
            if (mask[i]) {
                corr0 += env[i] * env[i];
                pairs0++;
            }
        }
        if (pairs0 > 0) corr0 /= pairs0;

        const float prev_bpm = raw_bpm; // use the RAW winner, not the smoothed display value
        float scores[ENV_SIZE] = {};
        float best_score = 0.0f;
        int   best_lag   = 0;
        for (int lag = lag_min; lag <= lag_max; ++lag) {
            float h = corr[lag];
            const float base = std::max(0.0f, corr[lag]);
            // ASYMMETRIC 2x harmonic: only use the double-period correlation when
            // the double-lag falls OUTSIDE the candidate range (2*lag > lag_max).
            // The beat level (e.g. lag 36 at 72 BPM, double=72) uses its 2x
            // harmonic because lag 72 > lag_max=44 -- it's not a competing tempo.
            // The eighth-note level (lag 18 at 144 BPM, double=36) CANNOT because
            // lag 36 < lag_max -- it IS a competing candidate (the beat itself!).
            // Previously the 2x bonus was unconditional, letting the subdivision
            // borrow the beat's correlation and making both levels score equally.
            // This asymmetry also pins the beat's score peak at the correct lag
            // (its harmonics align with the autocorrelation comb), reducing drift.
            if (2 * lag > lag_max && 2 * lag <= corr_lags)
                h += 0.50f * std::clamp(corr[2 * lag], 0.0f, base);
            if (3 * lag <= corr_lags)
                h += 0.25f * std::clamp(corr[3 * lag], 0.0f, base);
            // SUBDIVISION SUPPORT: the beat has an eighth note below it; the eighth
            // does not have a sixteenth. This is the primary octave cue.
            const int half_lag = lag / 2;
            if (half_lag >= 1) {
                const float c_half = std::max(corr[half_lag], corr[(lag + 1) / 2]);
                h += 0.55f * std::clamp(c_half, 0.0f, base);
            }
            float bpm = 60.0f / (lag * frame_dur_s);
            // Perceptual preference centered at 84 BPM, ~1.2 octaves wide.
            float lw  = std::log2(bpm / 84.0f) / 1.2f;
            // Continuity prior width at 1.0 octave (was 1.5). An octave jump now
            // costs ~40% (was ~20%), which blocks most spurious octave flips while
            // still allowing genuine tempo changes (a 10% shift costs only ~0.3%).
            float cw  = std::log2(bpm / prev_bpm) / 1.0f;
            float score = h * std::exp(-0.5f * lw * lw) * std::exp(-0.5f * cw * cw);
            scores[lag] = score;
            if (score > best_score) { best_score = score; best_lag = lag; }
        }

        // OCTAVE-DOWN OVERRIDE: if the score-function winner is at a fast level
        // (e.g. eighth notes at 144 BPM), but doubling the lag (= half BPM) is
        // still within range AND has a genuine correlation peak, prefer the slower
        // pulse. Rationale: when both the beat and its subdivision produce strong
        // autocorrelation peaks, the musically correct "beat" is almost always the
        // slower one. A genuinely fast song (e.g. 150 BPM) has no real correlation
        // at 75 BPM, so the override won't fire.
        {
            const int double_lag = 2 * best_lag;
            if (double_lag > lag_min && double_lag < lag_max &&
                corr[double_lag] > 0.30f * std::max(0.0f, corr[best_lag])) {
                // Verify it's a local peak (not just incidental positive correlation)
                const bool peak_left  = corr[double_lag] >= corr[double_lag - 1];
                const bool peak_right = corr[double_lag] >= corr[double_lag + 1];
                if (peak_left && peak_right) {
                    EVLOG("TEMPO octave-down override: lag %d (%.1f BPM) -> lag %d (%.1f BPM), corr ratio %.2f",
                          best_lag, 60.0f / (best_lag * frame_dur_s),
                          double_lag, 60.0f / (double_lag * frame_dur_s),
                          corr[double_lag] / std::max(1e-9f, corr[best_lag]));
                    best_lag   = double_lag;
                    best_score = scores[double_lag];
                }
            }
        }

        // Only adopt a new estimate when there's REAL periodicity: the peak must be a
        // meaningful fraction of the envelope's own variance. Ambient noise produces
        // normalized peaks of ~0.05 by chance (1/sqrtN fluctuations) -- an entire run
        // locked onto a phantom 163 BPM from quiet-room noise (corr 0.04-0.5 vs the
        // 2-20 real music produces) and the continuity prior then held it for 35s.
        // Below the bar: hold the last tempo (keeps the haptic steady through
        // ambiguous passages, and stops noise from steering at all).
        const bool confident = best_lag > 0 && corr0 > 1e-9f && corr[best_lag] > 0.08f * corr0;
        if (confident) low_conf_streak = 0; else ++low_conf_streak;
        if (confident) {
            // Parabolic interpolation on the RAW CORRELATION (not the score). The
            // score includes the perceptual prior, which makes the curve asymmetric
            // around the peak -- interpolating on it systematically biases the lag
            // toward the prior center. On-device this appeared as a slow BPM drift
            // (72 -> 67 over 60s). The raw correlation is symmetric around the true
            // periodicity, so interpolation is unbiased.
            float lag_f = static_cast<float>(best_lag);
            if (best_lag > lag_min && best_lag < lag_max) {
                const float y1 = corr[best_lag - 1], y2 = corr[best_lag], y3 = corr[best_lag + 1];
                const float denom = y1 - 2.0f * y2 + y3;
                if (std::fabs(denom) > 1e-12f) {
                    const float delta = 0.5f * (y1 - y3) / denom;
                    if (delta > -0.5f && delta < 0.5f) lag_f += delta;
                }
            }
            float bpm  = 60.0f / (lag_f * frame_dur_s);
            raw_bpm = bpm; // unsmoothed -- feeds the continuity prior next time
            float smoothed = (1.0f - TEMPO_OUTPUT_ALPHA) * tracked_bpm.load(std::memory_order_relaxed)
                             + TEMPO_OUTPUT_ALPHA * bpm;
            tracked_bpm.store(smoothed, std::memory_order_relaxed);
            EVLOG("TEMPO autocorr lag=%.2f rawBPM=%.1f conf=%.2f -> tracked=%.1f",
                  lag_f, bpm, corr[best_lag] / corr0, smoothed);
        } else if (best_lag > 0) {
            EVLOG("TEMPO hold (conf %.3f < 0.08)", corr0 > 1e-9f ? corr[best_lag] / corr0 : 0.0f);
        }
    }

    void initialize_matrices() {
        key_profiles = Eigen::MatrixXd::Zero(24, 12);

        // Default to the contemporary band profile
        build_key_profiles(ProfileType::SHAATH);
        build_key_transition();
        reset_key_posterior();
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
        ELOG("created | build=%s | features=0x%x | chromaFFT=%d (%.1f Hz/bin @44100) | silenceThr=%.3f keyThr=%.3f onsetThr=%.3f",
             ENGINE_BUILD_TAG, features, CHROMA_FFT_SIZE, 44100.0f / CHROMA_FFT_SIZE,
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

        // Blank the frames during/after our own vibration so the engine doesn't
        // analyze its own motor buzz (see HAPTIC_SUPPRESS_FRAMES). The window is
        // capped at a THIRD of the beat period: with a fixed 230ms a fast (often
        // wrong) tempo estimate had the suppression swallowing ~60% of all frames,
        // which starved the key detector outright (zero KEY frames for 24s
        // on-device) -- a feedback bug where a bad tempo silenced the key.
        if (haptic_just_fired.exchange(false, std::memory_order_relaxed)) {
            const float bpm = std::max(30.0f, tracked_bpm.load(std::memory_order_relaxed));
            const float period_frames = (60.0f / bpm) * sr / num_samples;
            // Half the period (was a third): a 186ms window still let the buzz tail
            // through at moderate tempos -- on-device PEAKS stayed pinned at 190-195Hz.
            // Half keeps at least every other frame clean, so the key never starves.
            haptic_suppress_left = std::min(HAPTIC_SUPPRESS_FRAMES,
                                            std::max(3, static_cast<int>(period_frames / 2.0f)));
        }
        const bool frame_contaminated = haptic_suppress_left > 0;
        if (frame_contaminated) {
            --haptic_suppress_left;
            chroma_contaminated = true;
        }

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
        rms_ema += 0.05f * (rms - rms_ema);

        // Sustain gate: require MIN_SUSTAINED_FRAMES consecutive loud frames before
        // updating the key HMM. This rejects brief jiggle transients (1-3 frames)
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
                reset();
                EVLOG("--- long silence (~3s), key + tempo reset ---");
            }
        } else {
            silence_frames = 0;
        }
        output_muted.store(silence_frames >= OUTPUT_MUTE_FRAMES, std::memory_order_relaxed);

        const bool now_sustained = (sustained_frames >= MIN_SUSTAINED_FRAMES);
        prev_sustained = now_sustained;

        // Key detection fires every CHROMA_FFT_SIZE samples (~93ms) when sustained.
        // The 4096-point FFT resolves bass semitones: G2 (98 Hz) and F2 (87 Hz) are
        // 11 Hz apart -- only separable at 10.8 Hz/bin, not at 43 Hz/bin (1024-pt FFT).
        if (chroma_acc_fill >= CHROMA_FFT_SIZE) {
            const bool key_wanted = (active_features.load(std::memory_order_relaxed) & WEARINGAID_FEATURE_KEY) &&
                                    now_sustained && rms >= KEY_RMS_THRESHOLD;
            // A window containing motor-buzz frames is skipped outright -- even a few
            // contaminated frames put a strong fake F#/G/G# blob into the chroma.
            // If that happens a lot, say so: silent starvation looked like a dead
            // key detector on-device.
            if (key_wanted && chroma_contaminated && ++starved_windows >= 50) {
                starved_windows = 0;
                ELOG("KEY starved: 50 chroma windows lost to haptic suppression (tempo too fast?)");
            }
            if (key_wanted && !chroma_contaminated) {
                // Hann window, linear signal (NO nonlinear gain). The previous tanh(x*4)
                // soft-clipped the waveform and injected spurious odd harmonics (a note's
                // 5th and 3rd), smearing energy across pitch classes; the rectangular FFT
                // added spectral leakage on top. Both left every key scoring within ~0.5%
                // of the others. Windowing + linear input preserves the true spectral shape.
                for (int i = 0; i < CHROMA_FFT_SIZE; ++i) {
                    float w = 0.5f * (1.0f - std::cos(6.2831853072f * i / (CHROMA_FFT_SIZE - 1)));
                    chroma_fft_in[i] *= w;
                }
                pffft_transform_ordered(chroma_fft_setup, chroma_fft_in, chroma_fft_out, chroma_fft_work, PFFFT_FORWARD);

                // PEAK-PICKED chroma: only spectral bins that are LOCAL MAXIMA count,
                // with parabolic interpolation of the true peak frequency. Two reasons:
                // 1) Below ~250 Hz a 10.8 Hz bin is WIDER than a semitone, so summing
                //    every bin smeared any low-frequency energy across several adjacent
                //    pitch classes (the F#/G/G# blob that hijacked the key estimate).
                //    A real note is a sharp peak; interpolating its frequency to
                //    sub-bin accuracy assigns it to the RIGHT pitch class even down low.
                // 2) Broadband noise (rumble, room hum skirts, mic floor) has no sharp
                //    peaks, so it now contributes almost nothing instead of dominating.
                // Each accepted peak contributes its magnitude (not energy=mag^2, which
                // let a single loud bass note drown the rest of the harmony).
                const int half = CHROMA_FFT_SIZE / 2;
                const int band_hi = static_cast<int>(2000.0f * CHROMA_FFT_SIZE / sr);
                chroma_mag_buf[0] = 0.0f;
                for (int i = 1; i < half; ++i) {
                    float re = chroma_fft_out[2 * i];
                    float im = chroma_fft_out[2 * i + 1];
                    chroma_mag_buf[i] = std::sqrt(re * re + im * im);
                }

                // Drone rejection: update the leaky-minimum floor from the raw spectrum,
                // then subtract it (see drone_mag declaration for the rationale).
                if (!drone_primed) {
                    std::memcpy(drone_mag, chroma_mag_buf, sizeof(drone_mag));
                    drone_primed = true;
                }
                // The published drone (for the flux notch + PEAKS log) is the argmax
                // WITHIN the 80-2000 Hz chroma band: unrestricted, it latched onto
                // infrasonic rumble bins (10.8/53.8 Hz on-device) that the chroma
                // never uses, wasting the notch. Subtraction still covers all bins.
                const int band_lo = static_cast<int>(80.0f * CHROMA_FFT_SIZE / sr) + 1;
                float dmax = 0.0f;
                int   dmax_i = 0;
                for (int i = 1; i < half; ++i) {
                    const float raw = chroma_mag_buf[i];
                    drone_mag[i] = std::min(raw, drone_mag[i] * DRONE_RISE);
                    chroma_mag_buf[i] = std::max(0.0f, raw - DRONE_SUBTRACT * drone_mag[i]);
                    if (i >= band_lo && i <= band_hi && drone_mag[i] > dmax) { dmax = drone_mag[i]; dmax_i = i; }
                }

                float gmax = 0.0f; // max of the CLEANED spectrum within the 0-2000 Hz band
                for (int i = 1; i <= band_hi && i < half; ++i)
                    if (chroma_mag_buf[i] > gmax) gmax = chroma_mag_buf[i];

                // Published for the tempo path: spectral flux ignores this band so the
                // motor/hum cannot drive the beat tracker either.
                drone_freq_hz = dmax_i * (sr / CHROMA_FFT_SIZE);
                drone_active  = dmax > 0.25f * gmax && dmax > 1e-9f;

                struct Peak { float freq; float mag; };
                Peak peaks[96];
                int  n_peaks = 0;
                const float peak_floor = 0.02f * gmax; // -34 dB: ignore the micro-peak noise floor
                for (int i = 2; i < half - 1 && n_peaks < 96; ++i) {
                    const float y2 = chroma_mag_buf[i];
                    if (y2 < peak_floor) continue;
                    const float y1 = chroma_mag_buf[i - 1], y3 = chroma_mag_buf[i + 1];
                    if (y2 <= y1 || y2 < y3) continue; // not a local maximum
                    const float denom = y1 - 2.0f * y2 + y3;
                    float delta = 0.0f;
                    if (std::fabs(denom) > 1e-12f)
                        delta = std::clamp(0.5f * (y1 - y3) / denom, -0.5f, 0.5f);
                    const float freq = (i + delta) * (sr / CHROMA_FFT_SIZE);
                    // 80-2000 Hz fundamental band. The floor must include the bass
                    // register (E2~82, G2~98, C3~130) where chord ROOTS live; peak
                    // gating makes the low end safe to include. The 2000 Hz ceiling
                    // trims the harmonic-rich top end that smears pitch classes.
                    if (freq <= 80.0f || freq >= 2000.0f) continue;
                    // PITCH-GRID GATE: a tuned note lands within ~+/-20 cents of the
                    // semitone grid; machine tones don't care about the grid. The
                    // vibration motor sits at ~190 Hz -- 46 cents off F#3, 54 off G3 --
                    // and was the top peak in most windows, voting G/F# for minutes
                    // straight (C major read as alternating C/G). >35 cents off = not
                    // a note. Parabolic interpolation is <5 cents on clean peaks, so
                    // the gate doesn't bite real music.
                    const float pitch_f = 69.0f + 12.0f * std::log2(freq / 440.0f);
                    if (std::fabs(pitch_f - std::round(pitch_f)) > 0.35f) continue;
                    peaks[n_peaks++] = {freq, y2};
                }

                // HARMONIC COLLAPSE: a peak at an integer multiple of a stronger,
                // lower peak is (mostly) that note's harmonic, not a separate note --
                // count it at 25%. Without this, one loud E voted E across three
                // octaves (335/660/1305 Hz on-device) and out-shouted the actual
                // tonic: "E everywhere" matches E-as-tonic (Em) better than
                // E-as-third-of-C, which is how a C major song read as Em.
                Peak top_peaks[6] = {};
                Eigen::VectorXf chroma = Eigen::VectorXf::Zero(12);
                for (int p = 0; p < n_peaks; ++p) { // peaks are in ascending frequency
                    float m = peaks[p].mag;
                    for (int q = 0; q < p; ++q) {
                        const float ratio = peaks[p].freq / peaks[q].freq;
                        const float n = std::round(ratio);
                        if (n >= 2.0f && n <= 6.0f &&
                            std::fabs(ratio - n) < 0.04f * n &&
                            peaks[q].mag > 0.4f * peaks[p].mag) {
                            m *= 0.25f;
                            break;
                        }
                    }
                    // BASS-REGISTER WEIGHTING: the key's tonic lives in the bass --
                    // chord ROOTS sit below ~250 Hz while the melody sits above it.
                    // Unweighted, a melody that hangs on E out-shouted the C/G/F/A
                    // root motion underneath and the song read as Em even on
                    // full-band windows. Roots now count double.
                    const float reg_w = (peaks[p].freq < 250.0f) ? 2.2f : 1.0f;
                    const int pitch = static_cast<int>(std::round(69.0f + 12.0f * std::log2(peaks[p].freq / 440.0f)));
                    chroma(((pitch % 12) + 12) % 12) += m * reg_w;
                    for (int t = 0; t < 6; ++t) {
                        if (m > top_peaks[t].mag) {
                            for (int u = 5; u > t; --u) top_peaks[u] = top_peaks[u - 1];
                            top_peaks[t] = {peaks[p].freq, m};
                            break;
                        }
                    }
                }

                // Once a second, dump the dominant peaks so logcat shows WHAT the engine
                // is hearing in Hz -- mains hum sits pinned at 100/150/200 Hz (EU) or
                // 120/180/240 Hz (US); the vibration motor at ~160-205 Hz; real music
                // moves with the notes.
                if (++peaks_log_counter >= 10) {
                    peaks_log_counter = 0;
                    char pkbuf[256];
                    int  off = 0;
                    for (int t = 0; t < 6 && top_peaks[t].mag > 0.0f && off < (int)sizeof(pkbuf) - 64; ++t) {
                        const int pp = static_cast<int>(std::round(69.0f + 12.0f * std::log2(top_peaks[t].freq / 440.0f)));
                        off += std::snprintf(pkbuf + off, sizeof(pkbuf) - off, " %.1fHz:%s(%.2f)",
                                             top_peaks[t].freq, KEY_NAMES[((pp % 12) + 12) % 12],
                                             top_peaks[t].mag / top_peaks[0].mag);
                    }
                    if (drone_freq_hz > 0.0f)
                        off += std::snprintf(pkbuf + off, sizeof(pkbuf) - off, " | drone %.1fHz%s",
                                             drone_freq_hz, drone_active ? " ACTIVE" : "");
                    EVLOG("PEAKS%s", pkbuf);
                }

                chroma.array() += 1e-9f;
                chroma = chroma.array() / chroma.sum();

                // Tonality gate: skip when the chromagram is nearly uniform (noise/percussion).
                // Threshold is lower than the old energy-chroma 2/12 because magnitude
                // chroma is inherently flatter (uniform would be 1/12 ~ 0.083).
                const float chroma_peak = chroma.maxCoeff();

                // SOLO-MELODY GUARD: count the pitch classes actually sounding in THIS
                // window (pre-EMA). One lone note says almost nothing about the key --
                // E belongs to C, G, D, Em, Am and E alike -- yet a sustained solo E
                // read as full-strength "Em" evidence for 6+ seconds and stole a
                // correct C lock (confirmed with the vibration motor off, so this is
                // musical content, not noise). Windows with <3 active pitch classes
                // get proportionally less emission weight below; the posterior coasts
                // through a-cappella/solo-line passages instead of being steered.
                int active_pcs = 0;
                for (int k = 0; k < 12; ++k)
                    if (chroma(k) >= 0.10f) ++active_pcs;
                const double evidence_w = std::min(1.0, std::max(0.0, (active_pcs - 1) / 2.0));

                if (chroma_peak >= 1.4f / 12.0f) {
                    if (!chroma_primed) {
                        chroma_avg = chroma; // first real frame: skip blend with zeros
                        chroma_primed = true;
                        chroma_warmup = 1;
                    } else {
                        chroma_avg = (1.0f - CHROMA_EMA_ALPHA) * chroma_avg + CHROMA_EMA_ALPHA * chroma;
                        if (chroma_warmup < KEY_WARMUP_FRAMES) chroma_warmup++;
                    }

                    // Emission: Pearson correlation of the chroma against all 24 profiles.
                    Eigen::VectorXd c = chroma_avg.cast<double>();
                    c.array() -= c.mean();
                    const double cnorm = c.norm();
                    if (cnorm > 1e-12) {
                        c /= cnorm;
                        Eigen::VectorXd r = key_profiles_norm * c; // 24 Pearson r values in [-1, 1]

                        // HMM forward filter step: diffuse the posterior through the
                        // transition prior, then weigh in this frame's evidence.
                        Eigen::VectorXd post = (key_log_post.array() - key_log_post.maxCoeff()).exp().matrix();
                        post /= post.sum();
                        Eigen::VectorXd pred = key_transition.transpose() * post;
                        key_log_post = (pred.array().max(1e-300).log()
                                        + evidence_w * KEY_EMISSION_BETA * r.array()).matrix();
                        key_log_post.array() -= key_log_post.maxCoeff();
                        key_log_post = key_log_post.cwiseMax(-KEY_LOG_POST_CLAMP);

                        int best; key_log_post.maxCoeff(&best);
                        int instant; const double instant_r = r.maxCoeff(&instant);
                        double second_lp = -1e9;
                        for (int k = 0; k < 24; ++k)
                            if (k != best && key_log_post(k) > second_lp) second_lp = key_log_post(k);

                        if (chroma_warmup < KEY_WARMUP_FRAMES) {
                            // Still integrating -- report nothing yet (watch shows "Listening").
                        } else if (committed_key < 0) {
                            // -second_lp is the best-vs-runner-up gap (best is 0 after
                            // normalization). Hold off until the evidence separates.
                            if (-second_lp >= KEY_FIRST_LOCK_GAP) {
                                committed_key = best;
                                challenger_key = -1; challenger_count = 0;
                            }
                        } else if (best == committed_key) {
                            challenger_key = -1; challenger_count = 0;
                        } else {
                            const double need = keys_closely_related(best, committed_key)
                                                ? KEY_SWITCH_MARGIN_RELATED
                                                : KEY_SWITCH_MARGIN_NATS;
                            const bool decisive =
                                key_log_post(best) - key_log_post(committed_key) >= need;
                            if (decisive && best == challenger_key) {
                                if (++challenger_count >= KEY_SWITCH_FRAMES) {
                                    committed_key = best;
                                    challenger_key = -1; challenger_count = 0;
                                }
                            } else if (decisive) {
                                challenger_key = best; challenger_count = 1;
                            } else {
                                challenger_key = -1; challenger_count = 0;
                            }
                        }
                        current_estimated_key.store(committed_key, std::memory_order_relaxed);

                        EVLOG("KEY vote %-3s (r=%.3f) | hmm %-3s gap %.2f | committed %-3s | peak %.2f w%.1f warm %d | chroma C%.2f C#%.2f D%.2f D#%.2f E%.2f F%.2f F#%.2f G%.2f G#%.2f A%.2f A#%.2f B%.2f",
                              KEY_NAMES[instant], instant_r,
                              KEY_NAMES[best], -second_lp,
                              committed_key >= 0 ? KEY_NAMES[committed_key] : "--",
                              chroma_peak, evidence_w, chroma_warmup,
                              chroma_avg(0), chroma_avg(1), chroma_avg(2), chroma_avg(3),
                              chroma_avg(4), chroma_avg(5), chroma_avg(6), chroma_avg(7),
                              chroma_avg(8), chroma_avg(9), chroma_avg(10), chroma_avg(11));
                    }
                } else {
                    EVLOG("KEY skip (flat chroma, peak %.2f < %.2f)", chroma_peak, 1.4f / 12.0f);
                }
            }
            chroma_acc_fill = 0;
            chroma_contaminated = false;
        }

        if (active_features.load(std::memory_order_relaxed) & WEARINGAID_FEATURE_TEMPO) {
            const float frame_duration_s = static_cast<float>(num_samples) / sr;
            current_time_s += frame_duration_s;

            // 1. Onset-strength envelope via SPECTRAL FLUX: sum of positive change in
            //    each frequency bin vs the previous frame. Spikes on note attacks (where
            //    beats live), so the envelope has strong beat-level periodicity -- unlike
            //    gross RMS change, which only tracks slow swells and locked onto the
            //    wrong (sub-beat) period. Reuses the otherwise-idle 1024-pt FFT.
            for (int i = 0; i < num_samples; ++i) fft_in[i] = pcm_data[i];
            pffft_transform_ordered(fft_setup, fft_in, fft_out, fft_work, PFFFT_FORWARD);
            float flux = 0.0f;
            const float flux_bin_hz = sr / num_samples;
            for (int k = 1; k < num_samples / 2; ++k) {
                float re = fft_out[2 * k], im = fft_out[2 * k + 1];
                float mag = std::sqrt(re * re + im * im);
                float d = mag - prev_mag[k];
                // The vibration-motor band (~150-250 Hz) is excluded from the flux
                // PERMANENTLY -- this replaces time-blanking the envelope, which even
                // with mean imputation gated the autocorrelation in a pattern periodic
                // at the haptic rate and re-confirmed whatever BPM the watch was
                // already buzzing (8s pinned at lag exactly 14.00 on-device). Two bins
                // of 512 are nothing to broadband music onsets; they are everything to
                // the motor. The drone notch still covers other stationary tones.
                const float k_freq = k * flux_bin_hz;
                if (d > 0.0f && (k_freq < 150.0f || k_freq > 250.0f) &&
                    !(drone_active && std::fabs(k_freq - drone_freq_hz) < 30.0f))
                    flux += d;
                prev_mag[k] = mag;
            }
            // Music-presence gate: only music-level audio may enter the envelope.
            // Without it, ambient room sound steered the tempo before any music
            // played (conf 0.4-0.7 at ~150 BPM from an empty-room recording).
            const bool music_present = rms_ema >= KEY_RMS_THRESHOLD;

            // Ambient-only frames get the running MEAN imputed (non-informative after
            // DC removal). Motor frames write their real flux into the envelope, but
            // they are explicitly masked out using env_mask so they don't bias the
            // autocorrelation towards the haptic period.
            if (!music_present) {
                onset_env[env_idx] = flux_mean;
                env_mask[env_idx] = 0;
            } else {
                flux_mean += 0.05f * (flux - flux_mean);
                onset_env[env_idx] = flux;
                env_mask[env_idx] = frame_contaminated ? 0 : 1;
            }
            env_idx = (env_idx + 1) % ENV_SIZE;

            // 2. Re-estimate the tempo (period) ~4x/sec from the whole envelope --
            //    but only while music is actually playing. During ambient-only
            //    stretches the BPM holds and the haptic goes quiet (low_conf_streak).
            if (++frames_since_tempo >= TEMPO_CALC_PERIOD) {
                frames_since_tempo = 0;
                if (music_present) {
                    estimate_tempo_autocorr(frame_duration_s);
                } else if (low_conf_streak < 1000) {
                    ++low_conf_streak;
                }
            }

            // 3. Onset events only drive the beat-phase PLL now (period comes from the
            //    autocorrelation, which is far steadier than per-onset intervals).
            energy_buf_sum -= energy_buf[energy_buf_idx];
            energy_buf[energy_buf_idx] = rms;
            energy_buf_sum += rms;
            energy_buf_idx = (energy_buf_idx + 1) % ENERGY_BUF_SIZE;
            float avg_energy = energy_buf_sum / ENERGY_BUF_SIZE;

            bool is_onset = !frame_contaminated && music_present &&
                            (avg_energy > 1e-6f) &&
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
                    // Deadzone: ignore hits near +/-half-beat (clear syncopations) so eighth-note
                    // off-beats don't drag the phase; on-beat hits cluster near err~0 and lock.
                    if (std::fabs(err) < 0.35f * beat_period) {
                        predicted_beat_time_s += BEAT_PHASE_GAIN * err;
                    }
                }
                last_onset_time_s = current_time_s;
            }

            if (predicted_beat_time_s > 0.0f && current_time_s >= predicted_beat_time_s) {
                // Only vibrate while the tempo estimate is backed by real periodicity
                // (~16 estimates ~ 4s of grace). A 19s stretch of conf~0 on-device had
                // the watch metronoming a stale guess through an arrhythmic passage.
                if (low_conf_streak < 16)
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
        reset_key_posterior();
        chroma_avg = Eigen::VectorXf::Zero(12);
        chroma_primed = false;
        chroma_warmup = 0;
        committed_key = -1;
        challenger_key = -1;
        challenger_count = 0;
    }

    void reset() {
        chroma_avg = Eigen::VectorXf::Zero(12);
        chroma_primed = false;
        chroma_warmup = 0;
        committed_key = -1;
        challenger_key = -1; challenger_count = 0;
        chroma_acc_fill = 0;
        reset_key_posterior();
        current_estimated_key.store(-1, std::memory_order_relaxed);
        // Clear the onset envelope so the next piece's tempo isn't correlated
        // against stale rhythm from the previous one.
        std::fill(onset_env, onset_env + ENV_SIZE, 0.0f);
        std::fill(env_mask, env_mask + ENV_SIZE, static_cast<uint8_t>(0));
        predicted_beat_time_s = -1.0f;
        last_onset_time_s = -1.0f;
        
        // Reset tempo outputs
        raw_bpm = 120.0f;
        tracked_bpm.store(120.0f, std::memory_order_relaxed);
        low_conf_streak = 0;
        haptic_pending.store(false, std::memory_order_relaxed);
    }

    EngineOutput tick_tempo(float time_delta_seconds) {
        time_since_last_beat += time_delta_seconds;

        // Mute outputs only after ~0.5s of sustained silence (set on the audio thread),
        // NOT on one quiet 23ms frame -- instantaneous gating made the key/BPM display
        // flicker off in the gaps between notes mid-song.
        if (output_muted.load(std::memory_order_relaxed)) {
            haptic_pending.store(false, std::memory_order_relaxed);
            return EngineOutput{-1, 0.0f, 0};
        }

        // exchange atomically reads haptic_pending and resets it to false in one op
        const bool trigger = haptic_pending.exchange(false, std::memory_order_relaxed);
        const uint32_t features = active_features.load(std::memory_order_relaxed);

        // The watch will vibrate right now -- tell the audio thread to blank the next
        // few frames so the engine doesn't analyze its own motor buzz.
        if (trigger && (features & WEARINGAID_FEATURE_TEMPO))
            haptic_just_fired.store(true, std::memory_order_relaxed);

        // BPM is reported as 0 when tempo tracking is off so the UI can hide the
        // readout instead of showing a stale number.
        return EngineOutput{
            (features & WEARINGAID_FEATURE_KEY)
                ? current_estimated_key.load(std::memory_order_relaxed)
                : -1,
            (features & WEARINGAID_FEATURE_TEMPO)
                ? tracked_bpm.load(std::memory_order_relaxed)
                : 0.0f,
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
    return EngineOutput{-1, 0.0f, 0};
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

void engine_reset(EngineState* engine) {
    if (engine && engine->instance) engine->instance->reset();
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