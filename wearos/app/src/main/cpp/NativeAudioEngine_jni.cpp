#include <jni.h>
#include <android/log.h>
#include <oboe/Oboe.h>
#include "audio_engine.h"

#define LOG_TAG "NativeAudioEngineJNI"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

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
                ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
                ->setFormat(oboe::AudioFormat::Float)
                ->setChannelCount(1)
                ->setSampleRate(44100)
                ->setCallback(this); // Tell Oboe to send buffers to onAudioReady()

        oboe::Result result = builder.openStream(stream);
        if (result == oboe::Result::OK) {
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

    // 2. The critical callback: Oboe fires this automatically when the mic has data
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *audioStream, void *audioData, int32_t numFrames) override {
        auto *floatData = static_cast<float *>(audioData);

        // Feed the core engine directly—zero JNI overhead!
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

JNIEXPORT void JNICALL
Java_com_palindrome_wearingaid_NativeAudioEngine_destroyEngine(JNIEnv *env, jobject thiz, jlong handle) {
    auto* wrapper = reinterpret_cast<OboeEngineWrapper*>(handle);
    delete wrapper;
}

}