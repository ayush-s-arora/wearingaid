#include <jni.h>
#include <android/log.h>
#include <oboe/Oboe.h>
#include "audio_engine.h"

#define LOG_TAG "NativeAudioEngineJNI"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

class OboeEngineWrapper : public oboe::AudioStreamCallback {
public:
    EngineState* core_engine;
    std::shared_ptr<oboe::AudioStream> stream;
    bool is_recording = false;

    OboeEngineWrapper() {
        core_engine = engine_create();
    }

    ~OboeEngineWrapper() override {
        stop();
        if (core_engine) {
            engine_destroy(core_engine);
        }
    }

    void start() {
        if (is_recording) return;

        oboe::AudioStreamBuilder builder;
        builder.setDirection(oboe::Direction::Input)
                ->setPerformanceMode(oboe::PerformanceMode::None)
                ->setInputPreset(oboe::InputPreset::Unprocessed)
                ->setFormat(oboe::AudioFormat::Float)
                ->setChannelCount(1)
                ->setSampleRate(44100)
                // The engine hardcodes 44100 Hz in every frequency→pitch calc. setSampleRate
                // is only a REQUEST; the mic may natively run at 48000. Force Oboe to resample
                // to exactly 44100 so the engine's assumption always holds — otherwise every
                // detected pitch is shifted (48000/44100 ≈ 1.46 semitones of systematic error).
                ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
                ->setFramesPerCallback(1024) // matches EXPECTED_FRAME_SIZE in audio_engine.cpp
                ->setCallback(this); // Tell Oboe to send buffers to onAudioReady()

        oboe::Result result = builder.openStream(stream);
        if (result == oboe::Result::OK) {
            // Log what Oboe ACTUALLY gave us — requested params are not guaranteed.
            // If sampleRate != 44100 or framesPerCallback != 1024, the engine silently
            // misbehaves (wrong pitches, or frames rejected by the size guard).
            const int actual_rate = stream->getSampleRate();
            ALOGD("stream opened | sampleRate=%d (req 44100) | framesPerBurst=%d | framesPerCallback=%d (req 1024) | channels=%d | format=%d | inputPreset=%d | perfMode=%d",
                  actual_rate, stream->getFramesPerBurst(),
                  stream->getFramesPerCallback(), stream->getChannelCount(),
                  static_cast<int>(stream->getFormat()),
                  static_cast<int>(stream->getInputPreset()),
                  static_cast<int>(stream->getPerformanceMode()));
            // Hand the engine the REAL rate so its pitch/tempo math matches what the
            // mic actually delivers (the watch captures at 48000, not the requested 44100).
            engine_set_sample_rate(core_engine, actual_rate);
            stream->requestStart();
            is_recording = true;
        } else {
            ALOGE("Failed to open Oboe stream: %s", oboe::convertToText(result));
        }
    }

    void stop() {
        if (!is_recording || !stream) return;
        stream->requestStop();
        stream->close();
        is_recording = false;
    }

    int last_logged_frames = -1;

    // 2. The critical callback: Oboe fires this automatically when the mic has data
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *audioStream, void *audioData, int32_t numFrames) override {
        auto *floatData = static_cast<float *>(audioData);

        // The engine rejects any frame whose size != 1024. If Oboe delivers a different
        // size (it can, despite setFramesPerCallback), nothing processes and the watch
        // looks "deaf". Log the size once and whenever it changes so this is visible.
        if (numFrames != last_logged_frames) {
            ALOGD("onAudioReady numFrames=%d%s", numFrames,
                  numFrames != 1024 ? " (!= 1024 — engine will DROP these frames!)" : "");
            last_logged_frames = numFrames;
        }

        // Feed the core engine directly. Zero JNI overhead!
        engine_push_audio(core_engine, floatData, numFrames);

        return oboe::DataCallbackResult::Continue;
    }
};

// JNI bindings
extern "C" {

JNIEXPORT jlong JNICALL
Java_com_palindrome_wearingaid_NativeAudioEngine_createEngine(JNIEnv *env, jobject thiz) {
    auto* wrapper = new OboeEngineWrapper();
    return reinterpret_cast<jlong>(wrapper);
}

JNIEXPORT void JNICALL
Java_com_palindrome_wearingaid_NativeAudioEngine_startRecording(JNIEnv *env, jobject thiz, jlong handle) {
    auto* wrapper = reinterpret_cast<OboeEngineWrapper*>(handle);
    if (wrapper) wrapper->start();
}

JNIEXPORT void JNICALL
Java_com_palindrome_wearingaid_NativeAudioEngine_stopRecording(JNIEnv *env, jobject thiz, jlong handle) {
    auto* wrapper = reinterpret_cast<OboeEngineWrapper*>(handle);
    if (wrapper) wrapper->stop();
}

JNIEXPORT jboolean JNICALL
Java_com_palindrome_wearingaid_NativeAudioEngine_tickTempo(JNIEnv *env, jobject thiz, jlong handle, jfloat deltaSec) {
    auto* wrapper = reinterpret_cast<OboeEngineWrapper*>(handle);
    if (!wrapper) return JNI_FALSE;

    // Pass the tick command down to the core engine
    EngineOutput out = engine_tick_tempo(wrapper->core_engine, deltaSec);
    return out.trigger_haptic ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloatArray JNICALL
Java_com_palindrome_wearingaid_NativeAudioEngine_tickOutput(JNIEnv *env, jobject thiz, jlong handle, jfloat deltaSec) {
    jfloat output[3] = {0.0f, 120.0f, 0.0f};
    auto* wrapper = reinterpret_cast<OboeEngineWrapper*>(handle);
    if (wrapper) {
        EngineOutput out = engine_tick_tempo(wrapper->core_engine, deltaSec);
        output[0] = static_cast<jfloat>(out.estimated_key_index);
        output[1] = out.estimated_bpm;
        output[2] = static_cast<jfloat>(out.trigger_haptic);
    }

    jfloatArray result = env->NewFloatArray(3);
    env->SetFloatArrayRegion(result, 0, 3, output);
    return result;
}

JNIEXPORT jfloat JNICALL
Java_com_palindrome_wearingaid_NativeAudioEngine_getRms(JNIEnv *env, jobject thiz, jlong handle) {
    auto* wrapper = reinterpret_cast<OboeEngineWrapper*>(handle);
    if (!wrapper) return 0.0f;
    return static_cast<jfloat>(engine_get_rms(wrapper->core_engine));
}

JNIEXPORT void JNICALL
Java_com_palindrome_wearingaid_NativeAudioEngine_setGenre(JNIEnv *env, jobject thiz, jlong handle, jint genreCode) {
    auto* wrapper = reinterpret_cast<OboeEngineWrapper*>(handle);
    if (wrapper) engine_set_genre(wrapper->core_engine, genreCode);
}

JNIEXPORT void JNICALL
Java_com_palindrome_wearingaid_NativeAudioEngine_setFeatures(JNIEnv *env, jobject thiz, jlong handle, jint features) {
    auto* wrapper = reinterpret_cast<OboeEngineWrapper*>(handle);
    if (wrapper) engine_set_features(wrapper->core_engine, static_cast<uint32_t>(features));
}

JNIEXPORT void JNICALL
Java_com_palindrome_wearingaid_NativeAudioEngine_resetEngine(JNIEnv *env, jobject thiz, jlong handle) {
    auto* wrapper = reinterpret_cast<OboeEngineWrapper*>(handle);
    if (wrapper) engine_reset(wrapper->core_engine);
}

JNIEXPORT void JNICALL
Java_com_palindrome_wearingaid_NativeAudioEngine_destroyEngine(JNIEnv *env, jobject thiz, jlong handle) {
    auto* wrapper = reinterpret_cast<OboeEngineWrapper*>(handle);
    delete wrapper;
}

}
