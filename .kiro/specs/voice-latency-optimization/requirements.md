# Requirements Document

## Introduction

This feature optimizes the end-to-end voice interaction latency of the ESP32-based child help device. The current pipeline is fully serial and "batch/full-wait": text-to-speech (TTS) synthesizes the entire clip into memory before any playback begins, and speech recognition (ASR) records the full recording window before it connects to iFlytek and uploads the whole clip. A full round trip currently reaches roughly 30 seconds, which feels unacceptably slow for a child in a help scenario.

Measured bottlenecks from device logs:

- WebSocket handshake (TTS/ASR reconnect on every round): 130–830 ms each
- TTS synthesis (batch, collected to memory before playback): 5643 ms and 9320 ms of perceived silence
- Recording (configured 6 s): ~5950 ms (expected)
- ASR recognition (6 s clip, uploaded only after recording ends): 8610 ms (empty `''` result) and 7900 ms (`'over'` result)
- Audio upload via `postCallsAudio` (~500 KB raw PCM): 6551 ms (~76 KB/s uplink)

The optimization targets, in priority order, are: (1) TTS streaming playback, (2) ASR streaming recognition, (3) WebSocket/TLS session reuse, (4) audio compression before upload, and (5) a correctness fix for the first ASR result returning empty. HTTP Date-based time synchronization is already optimized (94 ms) and is explicitly out of scope.

The relevant source files are `main/tts_xfyun.c`, `main/tts_xfyun.h`, `main/asr_xfyun.c`, `main/asr_xfyun.h`, `main/audio_driver.c`, `main/audio_driver.h`, and `main/api_service.c` (`postCallsAudio`). `main/time_sync.c` is out of scope.

## Glossary

- **TTS_Client**: The text-to-speech module (`tts_xfyun.c`) that connects to the iFlytek online TTS WebSocket service, receives synthesized audio chunks, and produces 16 kHz / 16-bit / mono PCM audio.
- **ASR_Client**: The speech recognition module (`asr_xfyun.c`) that connects to the iFlytek IAT WebSocket service, sends audio frames, and receives recognized text.
- **Audio_Driver**: The audio subsystem (`audio_driver.c`) that manages the ES8311 codec and I2S channels for recording and playback.
- **Playback_Consumer**: The playback thread within Audio_Driver that reads PCM data from the audio queue and writes it to the I2S TX channel.
- **Audio_Queue**: The bounded producer-consumer buffer (ring buffer / FreeRTOS queue) that decouples the WebSocket callback thread (producer) from the Playback_Consumer (consumer).
- **Upload_Service**: The audio upload path (`api_post_calls_audio_mem` / `postCallsAudio` in `api_service.c`) that uploads recorded audio to the backend.
- **Session_Manager**: The component responsible for maintaining and reusing WebSocket/TLS connections to iFlytek across multiple TTS or ASR rounds.
- **First_Packet_Latency**: The elapsed time from initiating a TTS synthesis request to the first audio sample being written to the I2S TX channel.
- **Recognition_Tail_Latency**: The elapsed time from the end of recording to the ASR final recognition result being available.
- **Recording_Window**: The configured audio capture duration for a single ASR listen attempt (currently 6 seconds).
- **Round_Trip_Latency**: The total elapsed time for one interaction cycle, measured from the moment recording starts to the moment TTS playback of the response completes.
- **PCM**: Pulse-code modulation raw audio, 16 kHz / 16-bit / mono, as used by the iFlytek TTS and IAT services.

## Requirements

### Requirement 1: TTS Streaming Playback

**User Story:** As a child using the help device, I want the device to begin speaking almost immediately after a response is ready, so that I am not left waiting in silence for several seconds.

#### Acceptance Criteria

1. WHEN the TTS_Client receives an audio chunk from the iFlytek TTS WebSocket service, THE TTS_Client SHALL enqueue the decoded PCM chunk into the Audio_Queue before the final chunk arrives.
2. WHEN the first PCM chunk is available in the Audio_Queue, THE Playback_Consumer SHALL begin writing PCM samples to the I2S TX channel without waiting for subsequent chunks.
3. WHEN a TTS response of at least 2 seconds of audio is synthesized, THE TTS_Client SHALL achieve a First_Packet_Latency, measured from submission of the synthesis request to the first PCM samples being written to the I2S TX channel, of no more than 1500 ms.
4. WHILE audio chunks are still arriving from the TTS WebSocket service, THE Playback_Consumer SHALL continue playing already-received PCM samples.
5. IF the Audio_Queue is empty while more audio chunks are still expected, THEN THE Playback_Consumer SHALL wait for the next chunk for up to 3000 ms without terminating playback.
6. WHEN the final TTS audio chunk (status indicating completion) has been enqueued and the Audio_Queue has been fully drained, THE Playback_Consumer SHALL complete playback and release playback resources.
7. IF the TTS WebSocket service reports an error code before any audio chunk is enqueued, THEN THE TTS_Client SHALL abort playback and return an error status to the caller indicating synthesis failure.
8. IF the TTS WebSocket service reports an error code after one or more audio chunks have already been enqueued, THEN THE Playback_Consumer SHALL continue playing the enqueued chunks to completion.
9. WHEN playback is aborted due to a reported error, THE TTS_Client SHALL return to a ready state that permits an immediate subsequent synthesis request without requiring an explicit error-clear step.
10. THE TTS_Client SHALL preserve the existing 16 kHz / 16-bit / mono PCM output format for streamed playback.
11. IF the Audio_Queue remains empty for more than 3000 ms while more audio chunks are still expected, THEN THE Playback_Consumer SHALL abort playback, release playback resources, and return an error status to the caller.
12. WHILE the Audio_Queue holds 32 chunks, THE TTS_Client SHALL block further enqueue operations until space becomes available rather than discarding chunks.

### Requirement 2: ASR Streaming Recognition

**User Story:** As a child using the help device, I want my spoken words to be recognized right after I finish speaking, so that the device responds quickly instead of pausing for many seconds.

#### Acceptance Criteria

1. WHEN recording of an ASR listen attempt begins, THE ASR_Client SHALL establish the iFlytek IAT WebSocket connection concurrently with audio capture, completing establishment within 10000 ms.
2. WHILE recording is in progress, THE ASR_Client SHALL send each captured audio frame to the iFlytek IAT WebSocket service as it becomes available, before the next frame is captured.
3. WHEN the Recording_Window ends, THE ASR_Client SHALL send a final frame indicating the end of the audio stream.
4. WHEN the Recording_Window ends, THE ASR_Client SHALL achieve a Recognition_Tail_Latency, measured from the end of the Recording_Window to delivery of the final recognition result to the caller, of no more than 2000 ms.
5. IF sending an audio frame to the IAT WebSocket service fails, THEN THE ASR_Client SHALL stop capturing and sending further audio frames, discard any partially accumulated recognition text, and return an error status to the caller indicating an audio-frame transmission failure.
6. WHEN the iFlytek IAT service returns the final recognition result, THE ASR_Client SHALL provide the recognized UTF-8 text to the caller.
7. THE ASR_Client SHALL send audio frames in the 16 kHz / 16-bit / mono PCM format required by the iFlytek IAT service.
8. IF the iFlytek IAT WebSocket connection cannot be established within 10000 ms of recording start, THEN THE ASR_Client SHALL stop the ASR listen attempt and return an error status to the caller indicating a connection-establishment timeout.
9. IF the final recognition result is not received within 15000 ms after the final frame is sent, THEN THE ASR_Client SHALL stop the ASR listen attempt and return an error status to the caller indicating a recognition-result timeout.

### Requirement 3: WebSocket and TLS Session Reuse

**User Story:** As a device operator, I want repeated voice rounds to avoid re-establishing connections each time, so that per-round handshake latency is eliminated.

#### Acceptance Criteria

1. WHEN a TTS or ASR round completes successfully AND the WebSocket connection is open with no reported transport errors, THE Session_Manager SHALL retain the connection for reuse by the next round of the same service type.
2. WHEN a subsequent round of the same service type begins AND a retained connection for that service type is open, has no reported transport errors, and has been idle for less than the iFlytek idle timeout, THE Session_Manager SHALL reuse the existing connection without performing a new WebSocket or TLS handshake.
3. IF a retained connection is closed, has reported a transport error, or has been idle for a duration equal to or greater than the iFlytek idle timeout, THEN THE Session_Manager SHALL discard that connection and establish a new connection for the next round.
4. IF a reused connection fails on the first data exchange of a new round, THEN THE Session_Manager SHALL discard the connection, establish a new connection, retry the round once, and log an error indicating that reuse failed.
5. WHEN a round reuses a retained connection, THE Session_Manager SHALL complete per-round connection setup in no more than 50 ms.
6. WHEN the voice interaction session ends, THE Session_Manager SHALL close all retained connections and release their associated resources within 1000 ms.

### Requirement 4: Audio Compression Before Upload

**User Story:** As a child on a weak network, I want my recorded audio to upload quickly, so that the help message reaches responders without a long delay.

#### Acceptance Criteria

1. WHEN a completed recording is prepared for upload, THE Upload_Service SHALL encode the audio using a compressed format before transmission.
2. THE Upload_Service SHALL produce a compressed payload that is no larger than 20% of the raw PCM payload of the same recording.
3. WHEN the compressed audio is uploaded via `postCallsAudio`, THE Upload_Service SHALL include metadata identifying the audio encoding format used.
4. IF audio compression fails, THEN THE Upload_Service SHALL transmit no partial payload, retain the original recording, and return an error status to the caller indicating compression failure.
5. THE Upload_Service SHALL preserve the 16 kHz sample rate and mono channel of the recording such that the decoded compressed audio contains the same spoken words as the original recording.
6. WHEN a recording spanning the configured Recording_Window is compressed and uploaded, THE Upload_Service SHALL complete the upload within 2000 ms under the baseline uplink conditions.

### Requirement 5: First Recognition Empty-Result Correctness

**User Story:** As a child asking for help, I want the device to recognize my first attempt to speak, so that I do not have to repeat myself and wait through another slow cycle.

#### Acceptance Criteria

1. WHEN the first ASR listen attempt of an interaction begins, THE Audio_Driver SHALL discard only the microphone samples captured during the codec and I2S input chain warm-up interval, where the warm-up interval SHALL NOT exceed 50 milliseconds.
2. WHEN recording begins for an ASR listen attempt, THE Audio_Driver SHALL begin retaining valid speech samples within 50 milliseconds of the recording start and SHALL capture the full configured Recording_Window duration, where the Recording_Window is configurable between 1 second and 10 seconds.
3. WHEN a child speaks a non-empty utterance during the first listen attempt, with ambient noise at or below 60 dB SPL and the speaker within 1 meter of the microphone, THE ASR_Client SHALL return a non-empty recognized text result.
4. IF the ASR_Client returns an empty recognition result, THEN THE ASR_Client SHALL report the empty-result condition to the caller within 500 milliseconds and SHALL preserve the interaction state so that a retry attempt can be initiated without loss of context.

### Requirement 6: Preserve Interaction Correctness

**User Story:** As a device operator, I want the latency optimizations to preserve the existing voice interaction behavior, so that faster responses do not introduce regressions.

#### Acceptance Criteria

1. WHEN the optimized TTS pipeline plays a response, THE Playback_Consumer SHALL play the complete synthesized audio content from the first to the final audio sample with no silent interruption exceeding 50 milliseconds between consecutive audio segments.
2. WHEN the optimized ASR pipeline recognizes speech for a given audio input, THE ASR_Client SHALL produce a final recognition transcript identical to the transcript produced by the current batch pipeline for that same audio input.
3. WHERE an existing playback abort is requested during streaming TTS playback, THE Playback_Consumer SHALL stop audible output and release all playback resources within 200 milliseconds of the abort request.
4. THE optimized pipeline SHALL reduce the measured Round_Trip_Latency to no more than 12 seconds for a response containing up to 4 seconds of synthesized audio, measured from the end of user speech input to the first audible sample played by the Playback_Consumer.
5. IF the optimized TTS pipeline fails to synthesize or stream audio for a response, THEN THE Playback_Consumer SHALL stop playback, release all playback resources, and present an indication that audio playback failed.
6. IF the optimized ASR pipeline fails to produce a recognition result for an audio input, THEN THE ASR_Client SHALL discard any partial results and present an indication that recognition failed.
