#include "audio_engine.h"
#include <Eigen/Dense>
#include <iostream>

class HmmAudioEngine {
private:
    Eigen::VectorXd current_key_probabilities;
    float tracked_bpm;
    float time_since_last_beat;

public:
    HmmAudioEngine() {
        // Initialize 24 key states with equal probability
        current_key_probabilities = Eigen::VectorXd::Constant(24, 1.0 / 24.0);
        tracked_bpm = 120.0f; 
        time_since_last_beat = 0.0f;
    }

    void process_chroma(const float* chroma12) {
        // Map raw array pointer directly to an Eigen Vector
        Eigen::Map<const Eigen::VectorXf> chroma_vector(chroma12, 12);
        std::cout << "[C++] Received chroma frame. Vector norm: " << chroma_vector.norm() << std::endl;
    }

    EngineOutput tick_tempo(float time_delta_seconds) {
        time_since_last_beat += time_delta_seconds;
        
        EngineOutput output;
        output.estimated_key_index = 0; 
        output.estimated_bpm = tracked_bpm;
        output.trigger_haptic = 0;      
        
        return output;
    }
};

// --- C-API Implementations ---

struct EngineState {
    HmmAudioEngine* instance;
};

EngineState* engine_create() {
    return new EngineState{new HmmAudioEngine()};
}

void engine_push_chroma(EngineState* engine, const float* chroma12) {
    if (engine && engine->instance) {
        engine->instance->process_chroma(chroma12);
    }
}

EngineOutput engine_tick_tempo(EngineState* engine, float time_delta_seconds) {
    if (engine && engine->instance) {
        return engine->instance->tick_tempo(time_delta_seconds);
    }
    return EngineOutput{0, 120.0f, 0};
}

void engine_destroy(EngineState* engine) {
    if (engine) {
        delete engine->instance;
        delete engine;
    }
}