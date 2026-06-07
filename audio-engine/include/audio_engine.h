#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#ifdef __cplusplus
extern "C" { // C-compatible API for JNI and Swift
#endif

typedef struct EngineState EngineState;

// Send results back up to watch with struct
typedef struct {
    int estimated_key_index;   // 0-11 for Major, 12-23 for Minor
    float estimated_bpm;       // Tracked system BPM
    int trigger_haptic;        // 1 if the watch should vibrate right now, 0 otherwise
} EngineOutput;

EngineState* engine_create();

// Called when the mic gets a new FFT frame (for Key tracking)
void engine_push_chroma(EngineState* engine, const float* chroma12);

// Called when the audio thread detects a volume spike (for Tempo tracking)
EngineOutput engine_tick_tempo(EngineState* engine, float time_delta_seconds);

void engine_destroy(EngineState* engine);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_ENGINE_H