#include "audio_engine.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Generates a pure sine wave at a specific frequency
void generate_sine(std::vector<float>& buffer, float freq, float sample_rate) {
    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = std::sin(2.0f * M_PI * freq * (static_cast<float>(i) / sample_rate));
    }
}

void verify_engine_logic() {
    const int num_samples  = 1024;
    const float sample_rate = 44100.0f;
    const int convergence_frames = 20; // ~460ms, enough for Viterbi to stabilize
    std::vector<float> sine_wave(num_samples);

    // --- Test 1: A4 (440Hz) with SHAATH ---
    {
        EngineState* engine = engine_create();
        generate_sine(sine_wave, 440.0f, sample_rate);
        for (int i = 0; i < convergence_frames; ++i)
            engine_push_audio(engine, sine_wave.data(), num_samples);
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
        generate_sine(sine_wave, 440.0f, sample_rate);
        for (int i = 0; i < convergence_frames; ++i)
            engine_push_audio(engine, sine_wave.data(), num_samples);
        EngineOutput out = engine_tick_tempo(engine, 0.1f);
        std::cout << "[Test 2] A4 EDMA key index: " << out.estimated_key_index
                  << " (expected 9 or 21)" << std::endl;
        assert(out.estimated_key_index == 9 || out.estimated_key_index == 21);
        engine_destroy(engine);
    }

    // --- Test 3: C4 (261.63Hz) with SHAATH ---
    {
        EngineState* engine = engine_create();
        generate_sine(sine_wave, 261.63f, sample_rate);
        for (int i = 0; i < convergence_frames; ++i)
            engine_push_audio(engine, sine_wave.data(), num_samples);
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

int main() {
    std::cout << "--- Starting Strict DSP Logic Verification ---" << std::endl;
    verify_engine_logic();
    verify_tempo_logic();
    std::cout << "--- Verification Complete: Engine Math is Stable ---" << std::endl;
    return 0;
}