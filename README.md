# WearingAid: A Wearable Approach to Supporting Deaf Musicians in Bands

## What
A WearOS (perhaps eventual expansion to watchOS) application for Deaf musicians in live performance scenarios.

## Why
Bands rely on auditory cues to ensure they stay in time with their bandmates and change keys on time, especially in improvised scenarios like jazz. However, members of the Deaf community likely struggle to hear their bandmates while performing, with substantial ambient noise inhibiting audition in certain venues and the transition between changing keys being more challenging to interpret due to perceived loudness discrepancies. This project seeks to address these challenges by providing an accessible tool that Deaf band members, irrespective of their instrument of choice, can install for assistance in live music scenarios.

## Architecture
**companion/** contains the Next.js frontend for companion devices to adjust the watch app's configuration. Companion-supported configuration details include a listener toggle, genre selection, feature toggles for tempo and key tracking, vibration sensitivity for tempo tracking, and RGB color selection for individual keys.

**wearos/** contains the WearOS implementation. It is written in Kotlin and connects to the C++ audio engine via the Java Native Interface (JNI). No audio processing occurs in this folder.

**audio-engine/** contains the C++ logic that powers tempo and key detection. `include/audio_engine.h` specifies the methods, exported in C for simplicity, that the wearable employs to transmit data to the engine. Additionally, this engine utilizes the following third party libraries:
- Eigen 5.0.0: Efficient matrix multiplication
- pffft: A fork of Julien Pommier's Pretty Fast FFT (PFFFT) library by Márton Danóczy {github link: https://github.com/marton78/pffft}; lightweight, wearable-ready fast fourier transforms


## Architectural Decisions
### Why PFFFT?
- pffft automatically detects ARM NEON to run instructions with SIMD, and ARM architecture is predominant in the smartwatch market. pffft is the natural choice for a wearable-first approach
### Why not run inference on the companion?
1. Companion is recommended, not required: If I required inference to occur on the companion device, having a companion in the first place would be mandatory. I sought to make this application wearble-first.
2. BLE: the default ~20 byte packet limit (a 23 byte ATT MTU minus a 3 byte header) prevents constant audio/detection result transfer between watch and companion.
3. UDP: Even if I decided against BLE and employed UDP, the constant stream would tank the battery of most smartwatches. A constant rich stream is impractical for most real-world live-music cases.
### Why not bundle the audio engine with the WearOS implementation?
I decided to develop the audio engine separately from the WearOS application for potential future watchOS support. Both applications would employ the same engine in their respective frontend.
### What optimizations do you make to minimize latency?
1. Microphone buffers are handed straight from the Oboe callback into the C++ engine, so there is no per-buffer JNI marshaling and no audio processing in the Kotlin layer. Detection results are passed back across threads with lock-free atomics, and the FFT setups and buffers are allocated once and reused, so nothing allocates on the audio path.
2. The WearOS application streams provides raw audio to the engine via Google Oboe {github link: https://github.com/google/oboe}, which is a C++ library designed for high-performance, low-latency audio applications on Android. It is the newest Android standard for this kind of application.
### How does the audio engine work?
1. Tempo: every frame, the engine computes a spectral flux onset strength (how much the spectrum jumps from one frame to the next, which spikes on note attacks) and buffers it over roughly 6 seconds. Autocorrelating that envelope finds the lag where the beat repeats, which gives the tempo, and a phase-locked clock nudges the wrist vibration onto the beat instead of letting it free-run.
2. Key detection: audio is run through a 4096 point FFT (Hann windowed) and the energy is folded into a 12 bin chromagram, one bin per pitch class, over the ~100-2000 Hz range where chord roots live. That chroma is smoothed over a few seconds so it reflects the song's key rather than each passing chord, then matched against 24 major and minor key profiles, with the best match winning. A short hold (hysteresis) keeps the displayed key from flickering between closely related keys.
### What is the impact of genre selection?
Each genre corresponds with a different key profile in the audio engine. The engine matches the detected chroma against the selected profile to identify the key, and they differ because different genres emphasize scale degrees differently (for instance, electronic music leans heavily on the tonic and fifth, while jazz and classical voicings spread weight across more of the scale). Profile-specific implementation inspired by Dr. Emilia G´omez Guti'errez's doctoral dissertation, TONAL DESCRIPTION OF MUSIC AUDIO SIGNALS. Profiles derived from the Music Technology Group - Universitat Pompeu Fabra's Essentia library {github link: https://github.com/MTG/essentia/blob/master/src/algorithms/tonal/key.cpp}.
