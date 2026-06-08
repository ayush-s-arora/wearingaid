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

int main() {
    std::cout << "--- Starting Strict DSP Logic Verification ---" << std::endl;
    verify_engine_logic();
    std::cout << "--- Verification Complete: Engine Math is Stable ---" << std::endl;
    return 0;
}