#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H
#define WEARINGAID_FEATURE_KEY    (1 << 0)
#define WEARINGAID_FEATURE_TEMPO  (1 << 1)
#include <stdint.h>

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

EngineState* engine_create(uint32_t features = WEARINGAID_FEATURE_KEY | WEARINGAID_FEATURE_TEMPO);

// Called with raw PCM samples from the microphone for each audio frame
void engine_push_audio(EngineState* engine, const float* pcm_data, int num_samples);

// Called when the audio thread detects a volume spike for onset detection
EngineOutput engine_tick_tempo(EngineState* engine, float time_delta_seconds);
void engine_set_verbose(int verbose);
void engine_set_genre(EngineState* engine, int genre_code); // 0 Temperley, 1 Shaath, 2 EDMA, 3 Wei Chai, 4 Tonic Triad
void engine_set_features(EngineState* engine, uint32_t features); // bitmask of WEARINGAID_FEATURE_*
void engine_reset(EngineState* engine);

// Tell the engine the ACTUAL capture sample rate (Hz) the audio backend delivers.
// All frequency→pitch and tempo timing math depends on this; the hardcoded default
// (44100) is wrong on devices that capture at 48000, shifting every detected pitch
// ~1.47 semitones and tempo ~8%. Call once after the stream opens.
void engine_set_sample_rate(EngineState* engine, int sample_rate);

float engine_get_rms(EngineState* engine);

void engine_destroy(EngineState* engine);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_ENGINE_H