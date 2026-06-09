package com.palindrome.wearingaid.presentation

import androidx.lifecycle.ViewModel
import com.palindrome.wearingaid.EngineSnapshot
import com.palindrome.wearingaid.NativeAudioEngine

class AudioViewModel : ViewModel() {
    private val audioEngine = NativeAudioEngine()

    fun startRecording() {
        audioEngine.start()
    }

    fun stopRecording() {
        audioEngine.stop()
    }

    fun checkTempo(deltaSec: Float): Boolean {
        return audioEngine.tick(deltaSec)
    }

    fun readOutput(deltaSec: Float): EngineSnapshot {
        return audioEngine.readOutput(deltaSec)
    }

    fun setGenre(genreCode: Int) {
        audioEngine.selectGenre(genreCode)
    }

    fun setFeatures(features: Int) {
        audioEngine.updateFeatures(features)
    }

    fun restartRecording() {
        audioEngine.stop()
        audioEngine.start()
    }

    fun getDebugPacket(keyIndex: Int, bpm: Float): String {
        val rms = audioEngine.getRms()
        return "k=$keyIndex,b=${bpm.toInt()},r=${"%.3f".format(rms)}"
    }

    // Prevents C++ memory leaks when the ViewModel is destroyed
    override fun onCleared() {
        super.onCleared()
        audioEngine.stop()
        audioEngine.release()
    }
}
