#include "audio_engine.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char* const KEY_NAMES_MAIN[24] = {
    "C",  "C#", "D",  "D#", "E",  "F",  "F#", "G",  "G#", "A",  "A#", "B",
    "Cm", "C#m","Dm", "D#m","Em", "Fm", "F#m","Gm", "G#m","Am", "A#m","Bm"
};

// Offline evaluation: stream a WAV file through the engine exactly as the watch
// would (1024-sample frames + a tick per frame) and print the key/BPM timeline.
// This removes the watch, mic, room, and vibration motor from the tuning loop —
// run `./test_engine song.wav` against a file whose key/tempo you KNOW.
// Supports canonical 16-bit PCM WAV, mono or stereo, any sample rate.
static int run_wav(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }

    char id[4]; uint32_t chunk_size = 0;
    if (std::fread(id, 1, 4, f) != 4 || std::memcmp(id, "RIFF", 4) != 0 ||
        std::fread(&chunk_size, 4, 1, f) != 1 ||
        std::fread(id, 1, 4, f) != 4 || std::memcmp(id, "WAVE", 4) != 0) {
        std::fprintf(stderr, "%s: not a RIFF/WAVE file\n", path);
        std::fclose(f);
        return 1;
    }

    uint16_t audio_format = 0, channels = 0, bits = 0;
    uint32_t sample_rate = 0;
    long     data_pos = 0;
    uint32_t data_len = 0;

    while (std::fread(id, 1, 4, f) == 4 && std::fread(&chunk_size, 4, 1, f) == 1) {
        if (std::memcmp(id, "fmt ", 4) == 0) {
            uint32_t byte_rate; uint16_t block_align;
            std::fread(&audio_format, 2, 1, f);
            std::fread(&channels, 2, 1, f);
            std::fread(&sample_rate, 4, 1, f);
            std::fread(&byte_rate, 4, 1, f);
            std::fread(&block_align, 2, 1, f);
            std::fread(&bits, 2, 1, f);
            std::fseek(f, static_cast<long>(chunk_size) - 16 + (chunk_size & 1), SEEK_CUR);
        } else if (std::memcmp(id, "data", 4) == 0) {
            data_pos = std::ftell(f);
            data_len = chunk_size;
            std::fseek(f, static_cast<long>(chunk_size) + (chunk_size & 1), SEEK_CUR);
        } else {
            std::fseek(f, static_cast<long>(chunk_size) + (chunk_size & 1), SEEK_CUR);
        }
    }

    if (audio_format != 1 || bits != 16 || channels < 1 || channels > 2 || data_pos == 0) {
        std::fprintf(stderr, "%s: need 16-bit PCM mono/stereo WAV (got fmt=%u bits=%u ch=%u)\n",
                     path, audio_format, bits, channels);
        std::fclose(f);
        return 1;
    }

    std::printf("WAV: %u Hz, %u ch, %.1f s\n", sample_rate, channels,
                static_cast<double>(data_len) / (sample_rate * channels * 2));

    EngineState* engine = engine_create();
    engine_set_sample_rate(engine, static_cast<int>(sample_rate));

    const int FRAME = 1024;
    std::vector<int16_t> raw(FRAME * channels);
    std::vector<float>   pcm(FRAME);
    const float frame_dur = static_cast<float>(FRAME) / sample_rate;

    std::fseek(f, data_pos, SEEK_SET);
    uint32_t samples_left = data_len / (2 * channels);
    float t = 0.0f, next_print = 1.0f;
    EngineOutput out{-1, 0.0f, 0};

    while (samples_left >= static_cast<uint32_t>(FRAME)) {
        if (std::fread(raw.data(), 2 * channels, FRAME, f) != static_cast<size_t>(FRAME)) break;
        for (int i = 0; i < FRAME; ++i) {
            if (channels == 2)
                pcm[i] = (raw[2 * i] + raw[2 * i + 1]) * (0.5f / 32768.0f);
            else
                pcm[i] = raw[i] / 32768.0f;
        }
        engine_push_audio(engine, pcm.data(), FRAME);
        out = engine_tick_tempo(engine, frame_dur);
        samples_left -= FRAME;
        t += frame_dur;
        if (t >= next_print) {
            next_print += 1.0f;
            std::printf("t=%6.1fs  key=%-3s  bpm=%6.1f\n", t,
                        out.estimated_key_index >= 0 ? KEY_NAMES_MAIN[out.estimated_key_index] : "--",
                        out.estimated_bpm);
        }
    }
    std::printf("FINAL: key=%s bpm=%.1f\n",
                out.estimated_key_index >= 0 ? KEY_NAMES_MAIN[out.estimated_key_index] : "--",
                out.estimated_bpm);

    engine_destroy(engine);
    std::fclose(f);
    return 0;
}

// Generates a pure sine wave at a specific frequency
void generate_sine(std::vector<float>& buffer, float freq, float sample_rate) {
    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = std::sin(2.0f * M_PI * freq * (static_cast<float>(i) / sample_rate));
    }
}

// Generates a MAJOR TRIAD on the given root. The engine's solo-melody guard
// (correctly) treats a lone sine as key-ambiguous and gives it no emission
// weight, so key tests must present an actual chord: root + major third + fifth
// is unambiguous (the third also pins major vs minor, so "root major" must win).
void generate_triad(std::vector<float>& buffer, float root_freq, float sample_rate) {
    const float third = root_freq * std::pow(2.0f, 4.0f / 12.0f);
    const float fifth = root_freq * std::pow(2.0f, 7.0f / 12.0f);
    for (size_t i = 0; i < buffer.size(); ++i) {
        const float t = static_cast<float>(i) / sample_rate;
        buffer[i] = 0.35f * (1.0f * std::sin(2.0f * M_PI * root_freq * t) +
                             0.8f * std::sin(2.0f * M_PI * third * t) +
                             0.9f * std::sin(2.0f * M_PI * fifth * t));
    }
}

// The engine's drone rejection erases any STATIONARY tone (a constant sine is
// indistinguishable from mains hum or motor whine), so the tests modulate the
// amplitude window-to-window — like a note being re-struck — to pass as music.
void push_modulated(EngineState* engine, const std::vector<float>& sine_wave, int frames) {
    std::vector<float> scaled(sine_wave.size());
    for (int i = 0; i < frames; ++i) {
        // One chroma window = 4 pushes of 1024; alternate loud/soft per window.
        const float scale = ((i / 4) % 2 == 0) ? 1.0f : 0.4f;
        for (size_t s = 0; s < sine_wave.size(); ++s) scaled[s] = sine_wave[s] * scale;
        engine_push_audio(engine, scaled.data(), static_cast<int>(sine_wave.size()));
    }
}

void verify_engine_logic() {
    const int num_samples  = 1024;
    const float sample_rate = 44100.0f;
    // Key warm-up needs KEY_WARMUP_FRAMES (10) chroma frames, then the first lock
    // waits for a 2-nat posterior gap (KEY_FIRST_LOCK_GAP). Each chroma frame is
    // 4096 samples = 4 pushes, and only the loud half of the modulation
    // contributes; a major triad separates "root major" decisively (the third
    // pins major vs minor), so 200 pushes clears both gates with ample margin.
    const int convergence_frames = 200;
    std::vector<float> sine_wave(num_samples);

    // --- Test 1: A4 (440Hz) with SHAATH ---
    {
        EngineState* engine = engine_create();
        generate_triad(sine_wave, 440.0f, sample_rate);
        push_modulated(engine, sine_wave, convergence_frames);
        EngineOutput out = engine_tick_tempo(engine, 0.1f);
        std::cout << "[Test 1] A4 SHAATH key index: " << out.estimated_key_index
                  << " (expected 9=A major or 21=A minor)" << std::endl;
        assert(out.estimated_key_index == 9 || out.estimated_key_index == 21);
        engine_destroy(engine);
    }

    // --- Test 2: A4 with EDMA ---
    {
        EngineState* engine = engine_create();
        engine_set_genre(engine, 2);
        generate_triad(sine_wave, 440.0f, sample_rate);
        push_modulated(engine, sine_wave, convergence_frames);
        EngineOutput out = engine_tick_tempo(engine, 0.1f);
        std::cout << "[Test 2] A4 EDMA key index: " << out.estimated_key_index
                  << " (expected 9 or 21)" << std::endl;
        assert(out.estimated_key_index == 9 || out.estimated_key_index == 21);
        engine_destroy(engine);
    }

    // --- Test 3: C4 (261.63Hz) with SHAATH ---
    {
        EngineState* engine = engine_create();
        generate_triad(sine_wave, 261.63f, sample_rate);
        push_modulated(engine, sine_wave, convergence_frames);
        EngineOutput out = engine_tick_tempo(engine, 0.1f);
        std::cout << "[Test 3] C4 SHAATH key index: " << out.estimated_key_index
                  << " (expected 0=C major or 12=C minor)" << std::endl;
        assert(out.estimated_key_index == 0 || out.estimated_key_index == 12);
        engine_destroy(engine);
    }
}

void verify_tempo_logic() {
    const int   num_samples     = 1024;
    const float sample_rate     = 44100.0f;
    const float frame_duration  = num_samples / sample_rate; // ~0.023s
    const float target_bpm      = 120.0f;
    const float beat_interval_s = 60.0f / target_bpm;       // 0.5s
    const int   frames_per_beat = static_cast<int>(std::round(beat_interval_s / frame_duration)); // ~22

    std::vector<float> beat_frame(num_samples);
    std::vector<float> quiet_frame(num_samples);

    srand(42);
    for (auto& s : beat_frame)  s = (rand() / (float)RAND_MAX) * 2.0f - 1.0f;   // loud burst
    for (auto& s : quiet_frame) s = (rand() / (float)RAND_MAX) * 0.01f - 0.005f; // near silence

    EngineState* engine = engine_create();
    int haptic_count = 0;

    // Simulate 10 beats worth of audio
    const int total_frames = frames_per_beat * 10;
    for (int frame = 0; frame < total_frames; ++frame) {
        bool is_beat = (frame % frames_per_beat == 0);
        engine_push_audio(engine, is_beat ? beat_frame.data() : quiet_frame.data(), num_samples);
        EngineOutput out = engine_tick_tempo(engine, frame_duration);
        if (out.trigger_haptic) haptic_count++;
    }

    EngineOutput final_out = engine_tick_tempo(engine, 0.0f);
    std::cout << "[Test] Estimated BPM: " << final_out.estimated_bpm
              << " (target: " << target_bpm << ")" << std::endl;
    std::cout << "[Test] Haptic triggers fired: " << haptic_count << std::endl;

    // BPM within one step size of target
    assert(std::abs(final_out.estimated_bpm - target_bpm) <= 4.0f);
    // At least 6 haptics fired (giving grace for bootstrap frames)
    assert(haptic_count >= 6);

    engine_destroy(engine);
}

int main(int argc, char** argv) {
    bool verbose = false;
    const char* wav_path = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0 || std::strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else {
            wav_path = argv[i];
        }
    }

    // Apply the verbose flag globally for the C API
    engine_set_verbose(verbose ? 1 : 0);

    // With a WAV path: offline evaluation against known ground truth.
    if (wav_path) return run_wav(wav_path);

    // Without arguments: the synthetic DSP verification suite.
    std::cout << "--- Starting Strict DSP Logic Verification ---" << std::endl;
    verify_engine_logic();
    verify_tempo_logic();
    std::cout << "--- Verification Complete: Engine Math is Stable ---" << std::endl;
    return 0;
}