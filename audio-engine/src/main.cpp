#include "audio_engine.h"
#include <iostream>

int main() {
    std::cout << "--- Starting WearOS Audio Engine Test ---\n" << std::endl;

    EngineState* engine = engine_create();

    // 2. Simulate a microphone detecting a C-Major chord (C, E, G are loud, rest are quiet)
    // Index:  C     C#    D     D#    E     F     F#    G     G#    A     A#    B
    float mock_chroma[12] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    
    std::cout << "[Test] Pushing mock C-Major chroma frame..." << std::endl;
    engine_push_chroma(engine, mock_chroma);

    // 3. Simulate an onset detector finding a beat after 0.5 seconds (120 BPM)
    std::cout << "[Test] Simulating 0.5s beat interval..." << std::endl;
    EngineOutput output = engine_tick_tempo(engine, 0.5f);

    // 4. Print the resulting state
    std::cout << "\n[Engine Output]" << std::endl;
    std::cout << "Estimated Key Index: " << output.estimated_key_index << std::endl;
    std::cout << "Estimated BPM:       " << output.estimated_bpm << std::endl;
    std::cout << "Trigger Haptic:      " << output.trigger_haptic << "\n" << std::endl;

    // 5. Prevent memory leaks
    engine_destroy(engine);
    
    std::cout << "--- Test Complete ---" << std::endl;
    return 0;
}