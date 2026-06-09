# WearingAid: A Wearable Approach to Supporting Deaf Musicians in Bands

## What
A WearOS (with eventual expansion to watchOS) application for Deaf musicians in live performance scenarios. WearingAid listens to the surrounding music on the watch's microphone and translates two things a hearing musician relies on — **the current key** and **the tempo** — into accessible signals: an on-watch color/label for the key, and a wrist vibration on every beat.

## Why
Bands rely on auditory cues to stay in time with their bandmates and to change keys on time, especially in improvised scenarios like jazz. Members of the Deaf community can struggle to hear their bandmates while performing: substantial ambient noise inhibits audition in many venues, and key changes are harder to interpret due to perceived-loudness discrepancies. This project addresses those challenges with an accessible tool that Deaf band members — irrespective of their instrument — can install for assistance in live music.

## Architecture
**companion/** contains the Next.js frontend for companion devices to adjust the watch app's configuration. Companion-supported configuration includes a listening toggle, genre selection, feature toggles for tempo and key tracking, vibration sensitivity for tempo tracking, and RGB color selection for individual keys. The companion talks to the watch over Bluetooth Low Energy (BLE).

**wearos/** contains the WearOS implementation. It is written in Kotlin and connects to the C++ audio engine via the Java Native Interface (JNI). No audio processing occurs in this folder — it captures microphone audio, forwards it to the engine, renders the UI, drives the haptics, and runs the BLE GATT server that the companion connects to.

**audio-engine/** contains the C++ logic that powers tempo and key detection. `include/audio_engine.h` specifies the methods, exported in C for simplicity, that the wearable uses to feed audio to and read results from the engine. This engine uses the following third-party libraries:
- **Eigen 5.0.0**: efficient matrix/vector math (chroma–key-profile correlation).
- **pffft**: a fork of Julien Pommier's Pretty Fast FFT (PFFFT) by Márton Danóczy ([github](https://github.com/marton78/pffft)); lightweight, wearable-ready fast Fourier transforms.

## Architectural Decisions

### Why PFFFT?
pffft automatically detects ARM NEON and runs SIMD instructions, and ARM is the predominant architecture in the smartwatch market. For a wearable-first approach, pffft is the natural choice — it's small, dependency-free, and fast on the exact hardware we target.

### Why not run inference on the companion?
1. **Companion is recommended, not required.** Requiring inference on the companion would make having a companion mandatory. WearingAid is wearable-first: it must work standalone on the watch.
2. **BLE bandwidth.** BLE's default ~20-byte notification payload (ATT MTU of 23 bytes, minus the 3-byte header) makes it impractical to continuously stream raw audio to the companion and detection results back. BLE here carries only small config packets and compact debug telemetry.
3. **Power.** Even if we dropped BLE for a higher-bandwidth transport (e.g. UDP over Wi-Fi), a constant rich audio/result stream would tank the battery of most smartwatches. A constant stream is impractical for real-world live-music use.

### Why not bundle the audio engine with the WearOS implementation?
The audio engine is developed separately from the WearOS application for potential future watchOS support. Both frontends can share the same engine, with each providing its own platform integration (audio capture, UI, haptics).

### What optimizations minimize latency?
1. **Zero-copy, zero-JNI-overhead audio path.** Microphone buffers are handed directly from the Oboe callback into the C++ engine (`engine_push_audio`) — there is no per-buffer JNI marshaling or audio processing in the Kotlin/Java layer.
2. **Low-latency native capture via Google Oboe.** The WearOS app streams raw audio to the engine via [Google Oboe](https://github.com/google/oboe), a C++ library designed for high-performance, low-latency audio on Android, using the `Unprocessed` input preset so the OS does not apply voice-oriented noise suppression that would destroy musical content.
3. **Lock-free cross-thread results.** Detection results are published from the audio callback thread and read from the UI thread through `std::atomic` values — no locks on the audio path.
4. **Pre-allocated DSP resources.** FFT setups and working buffers are allocated once at engine creation and reused on every callback, avoiding per-frame allocation.
5. **Off-main-thread reads.** The watch reads engine output on a background dispatcher so JNI calls never block the UI/render thread.

### How does the audio engine work?

**Tempo detection (autocorrelation of an onset envelope).**
Each ~23 ms audio frame, the engine computes a **spectral-flux onset strength** — the sum of positive frame-to-frame change across FFT magnitude bins, which spikes on note attacks. These values fill a ~6-second ring buffer. Roughly four times per second the engine **autocorrelates** that envelope: the lag at which the envelope best repeats is the beat period. A perceptual preference (centered near typical tempi) plus a gentle continuity term resolve metrical-level ambiguity (e.g. avoiding double/half-time) and keep the estimate stable. The resulting BPM is EMA-smoothed, and a **phase-locked beat clock** is nudged toward detected onsets so the wrist vibration lands *on* the beat rather than free-running.

**Key detection (chroma template matching).**
Audio is accumulated into a 4096-point FFT (~10.8 Hz/bin) for the frequency resolution needed to separate low semitones. A **Hann window** is applied, then spectral **energy is folded into a 12-bin chromagram** (one bin per pitch class) over the ~100–2000 Hz fundamental band — low enough to capture chord roots in the bass, where the tonal center lives. The chromagram is normalized and smoothed with a long (~4.5 s) exponential moving average so the estimate reflects the song's *global key* rather than each passing chord. That smoothed chroma is correlated (via Eigen) against 24 **key profiles** — major and minor templates circularly shifted to all 12 roots — and the best match is taken. A hysteresis stage requires a challenger key to hold a sustained lead before the displayed key switches, which suppresses flicker between closely related keys.

A silence gate (RMS threshold) and a tonality gate (the chroma must have a real peak, not be near-uniform noise) prevent the engine from reporting a key or beat when only ambient noise is present.

### What is the impact of genre selection?
Each genre selects a different **key profile** in the audio engine — the template of expected pitch-class weights that the chromagram is matched against. Genres differ because different musical traditions emphasize scale degrees differently (e.g. electronic music leans heavily on the tonic and fifth, while jazz/classical voicings spread weight across more of the scale), so a profile tuned to the genre improves key-match accuracy. The mapping is:

| Genre (companion) | Key profile |
|---|---|
| Classical (jazz, traditional) | Temperley |
| Band (rock, pop, live) | Shaath |
| Electronic (EDM, house, techno) | EDMA |
| Acoustic (folk, solo, traditional) | Wei Chai |
| Minimal (ambient, drone) | Tonic triad |

The profile approach is inspired by Dr. Emilia Gómez Gutiérrez's doctoral dissertation, *Tonal Description of Music Audio Signals*, and the profile values are derived from the Music Technology Group — Universitat Pompeu Fabra's [Essentia](https://github.com/MTG/essentia/blob/master/src/algorithms/tonal/key.cpp) library.

## Repository layout
```
companion/      Next.js + TypeScript companion web app (Web Bluetooth)
wearos/         WearOS app (Kotlin) + JNI bridge + Oboe capture
audio-engine/   Platform-agnostic C++ engine (key + tempo), C API in include/audio_engine.h
docs/           GitHub Pages site (this README, themed)
```
