package com.palindrome.wearingaid

class NativeAudioEngine {
    private var engineHandle: Long = 0

    init {
        System.loadLibrary("wearingaid_jni")
        engineHandle = createEngine()
    }

    external fun startRecording(handle: Long)
    external fun stopRecording(handle: Long)
    external fun tickTempo(handle: Long, deltaSec: Float): Boolean

    private external fun createEngine(): Long
    private external fun destroyEngine(handle: Long)

    // Helper functions to isolate Oboe logic from rest of app
    fun start() = startRecording(engineHandle)
    fun stop() = stopRecording(engineHandle)
    fun tick(deltaSec: Float): Boolean = tickTempo(engineHandle, deltaSec)

    fun release() {
        if (engineHandle != 0L) {
            destroyEngine(engineHandle)
            engineHandle = 0L
        }
    }
}