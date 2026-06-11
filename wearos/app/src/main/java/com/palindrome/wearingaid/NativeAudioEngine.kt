package com.palindrome.wearingaid

data class EngineSnapshot(
    val keyIndex: Int,
    val bpm: Float,
    val shouldVibrate: Boolean
)

class NativeAudioEngine {
    private var engineHandle: Long = 0

    init {
        System.loadLibrary("wearingaid_jni")
        engineHandle = createEngine()
    }

    external fun startRecording(handle: Long)
    external fun stopRecording(handle: Long)
    external fun tickTempo(handle: Long, deltaSec: Float): Boolean
    external fun tickOutput(handle: Long, deltaSec: Float): FloatArray
    external fun getRms(handle: Long): Float
    external fun setGenre(handle: Long, genreCode: Int)
    external fun setFeatures(handle: Long, features: Int)

    private external fun createEngine(): Long
    private external fun resetEngine(handle: Long)
    private external fun destroyEngine(handle: Long)

    // Helper functions to isolate Oboe logic from rest of app
    fun start() = startRecording(engineHandle)
    fun stop() = stopRecording(engineHandle)
    fun tick(deltaSec: Float): Boolean = tickTempo(engineHandle, deltaSec)
    fun selectGenre(genreCode: Int) = setGenre(engineHandle, genreCode)
    fun updateFeatures(features: Int) = setFeatures(engineHandle, features)
    fun reset() = resetEngine(engineHandle)
    fun getRms(): Float = getRms(engineHandle)
    fun readOutput(deltaSec: Float): EngineSnapshot {
        val output = tickOutput(engineHandle, deltaSec)
        // Defaults match the engine's "nothing detected" outputs: -1 = no key (0 would
        // render as C Major), 0 BPM = hide the readout (120 would show a fake tempo).
        return EngineSnapshot(
            keyIndex = output.getOrNull(0)?.toInt() ?: -1,
            bpm = output.getOrNull(1) ?: 0f,
            shouldVibrate = (output.getOrNull(2) ?: 0f) > 0f
        )
    }

    fun release() {
        if (engineHandle != 0L) {
            destroyEngine(engineHandle)
            engineHandle = 0L
        }
    }
}
