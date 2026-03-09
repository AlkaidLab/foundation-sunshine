# macOS AirType Remote Microphone Solution

## Date: 2026-03-03

## Problem

AirType (speech-to-text) requires specific audio format and signal level to recognize speech from BlackHole virtual device.

## Root Cause

When using HAL Output AudioUnit targeting an aggregate device, the AudioUnit defaults to the device's native format (often 44.1kHz mono) instead of our pipeline's format (48kHz stereo float32). This causes:
- Buffer size mismatch (1884 bytes instead of expected size)
- Format conversion issues
- AirType unable to recognize audio input

## Solution

**Explicitly set AudioUnit input stream format to match our audio pipeline:**

```objc
// Set stream format to match our audio pipeline (48kHz stereo float32 interleaved)
AudioStreamBasicDescription streamFormat = {};
streamFormat.mSampleRate = 48000.0;
streamFormat.mFormatID = kAudioFormatLinearPCM;
streamFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
streamFormat.mChannelsPerFrame = 2;
streamFormat.mBitsPerChannel = 32;
streamFormat.mBytesPerFrame = sizeof(float) * 2;  // 8 bytes per frame (stereo)
streamFormat.mFramesPerPacket = 1;
streamFormat.mBytesPerPacket = streamFormat.mBytesPerFrame;

AudioUnitSetProperty(audio_unit,
                     kAudioUnitProperty_StreamFormat,
                     kAudioUnitScope_Input, 0,
                     &streamFormat, sizeof(streamFormat));
```

## Key Points

1. **Format must match pipeline**: Our pipeline produces 48kHz stereo float32 interleaved
2. **Set before render callback**: Format must be set before registering the render callback
3. **HAL Output vs DefaultOutput**: HAL Output requires explicit format, DefaultOutput auto-negotiates
4. **AirType requirements**: Needs consistent format and sufficient signal level (15x gain boost)

## Audio Pipeline

```
Tablet mic → OPUS encoded → Network → Sunshine
  → OPUS decode (48kHz mono int16)
  → Convert to stereo float32 + 15x gain
  → TPCircularBuffer
  → AudioUnit render callback (48kHz stereo float32)
  → Single-BlackHole aggregate device
  → BlackHole input
  → AirType / WeChat
```

## Implementation Location

File: `src/platform/macos/microphone.mm`
Function: `init_mic_redirect_device()`
Lines: ~488-507 (stream format setting)

## Testing

1. Enable remote microphone in Sunshine
2. Check BlackHole input level: `ffmpeg -f avfoundation -i ":BlackHole 2ch" -t 5 -af volumedetect -f null -`
3. Expected: max_volume around -1 to -10 dB
4. Test AirType: Should recognize speech without "录制为空" error
5. Verify no electrical noise

## Related Issues

- Single-BlackHole aggregate device bypasses BlackHole's IOProc limitation
- No default output device change needed (preserves streaming audio)
- Zero electrical noise (no multi-device clock sync issues)
