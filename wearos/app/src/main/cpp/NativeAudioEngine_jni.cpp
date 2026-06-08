#include <jni.h>
#include <android/log.h>
#include "audio_engine.h"

#define LOG_TAG "NativeAudioEngineJNI"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_palindrome_wearingaid_NativeAudioEngine_createEngine(JNIEnv *env, jobject thiz) {
    EngineState* engine = engine_create();
    return reinterpret_cast<jlong>(engine); // Cast C++ pointer to a long for Kotlin
}

JNIEXPORT void JNICALL
Java_com_palindrome_wearingaid_NativeAudioEngine_pushAudio(JNIEnv *env, jobject thiz, jlong engineHandle, jfloatArray audioData) {
    auto* engine = reinterpret_cast<EngineState*>(engineHandle);
    if (!engine) {
        ALOGE("Engine handle is null during pushAudio");
        return;
    }

    // Get the length of the array
    jsize len = env->GetArrayLength(audioData);

    // Pin the array in memory and get a direct pointer to the floats
    jfloat *buffer = env->GetFloatArrayElements(audioData, nullptr);

    // jfloat is strictly a typedef for a standard C++ float, so this passes cleanly
    engine_push_audio(engine, buffer, len);

    // JNI_ABORT tells the JVM that we did not modify the array in C++.
    // This prevents the JVM from wasting cycles copying the array back to Kotlin memory.
    env->ReleaseFloatArrayElements(audioData, buffer, JNI_ABORT);
}

JNIEXPORT jboolean JNICALL
Java_com_palindrome_wearingaid_NativeAudioEngine_tickTempo(JNIEnv *env, jobject thiz, jlong engineHandle, jfloat deltaSec) {
    auto* engine = reinterpret_cast<EngineState*>(engineHandle);
    if (!engine) {
        ALOGE("Engine handle is null during tickTempo");
        return JNI_FALSE;
    }

    EngineOutput out = engine_tick_tempo(engine, deltaSec);

    // Convert the C++ boolean to the strict JNI boolean type
    return out.trigger_haptic ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_palindrome_wearingaid_NativeAudioEngine_destroyEngine(JNIEnv *env, jobject thiz, jlong engineHandle) {
    auto* engine = reinterpret_cast<EngineState*>(engineHandle);
    if (engine) {
        engine_destroy(engine);
    }
}

}