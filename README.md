# WearingAid: A Wearable Approach to Supporting Deaf Musicians in Bands

## What
A WearOS (perhaps eventual expansion to watchOS) application for Deaf musicians in live performance scenarios.

## Why
Bands rely on auditory cues to ensure they stay in time with their bandmates and change keys on time, especially in improvised scenarios like jazz. However, members of the Deaf community likely struggle to hear their bandmates while performing, with substantial ambient noise inhibiting audition in certain venues and the transition between changing keys being more challenging to interpret due to perceived loudness discrepancies. This project seeks to address these challenges by providing an accessible tool that Deaf band members, irrespective of their instrument of choice, can install for assistance in live music scenarios.

## Notes
**companion/** contains the Next.js frontend for companion devices to adjust the watch app's configuration

**wearos/** contains the WearOS implementation

**audio-engine/** contains the C++ logic that powers tempo and key detection

I decided to develop the audio engine separately from the WearOS application for potential future watchOS support. Both applications would employ the same engine in their frontends. 