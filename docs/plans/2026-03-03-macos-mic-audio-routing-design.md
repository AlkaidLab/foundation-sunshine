# macOS Remote Microphone Audio Routing Design

## Date: 2026-03-03

## Problem

Remote microphone on macOS requires writing audio to BlackHole virtual device.
BlackHole has a limitation: it only routes audio to its input side when used as the
system default output device. Direct IOProc and HAL AudioUnit writes produce silence
on BlackHole's input (-91 dB).

Setting BlackHole as default output breaks:
- Streaming video audio (Sunshine audio capture depends on default output)
- Other apps' audio output (WeChat voice calls, etc.)

## Requirements

- Streaming video audio must work normally
- Remote microphone must work (AirType, WeChat voice input)
- Zero electrical noise / buzzing
- System default output device must not be changed
- Clean session lifecycle (init on connect, release on disconnect)

## Design

### Primary Approach: Single-BlackHole Aggregate Device + HAL AudioUnit

1. Create aggregate device containing only BlackHole (no clock conflicts = no noise)
2. Use HAL Output AudioUnit targeting the aggregate device
3. Audio goes through AudioServer path (not raw IOProc), which may enable BlackHole routing
4. System default output stays unchanged — all other audio unaffected

### Fallback: DefaultOutput + Switch Default Output

If the aggregate approach doesn't route audio to BlackHole's input:
1. Save original default output device
2. Set BlackHole as default output
3. Use DefaultOutput AudioUnit
4. Restore original default output on session end

This approach is proven to work but temporarily disrupts other audio apps.

### Audio Pipeline

```
Tablet mic → OPUS encoded → Network → Sunshine
  → OPUS decode (48kHz mono int16)
  → Convert to stereo float32 + 15x gain
  → TPCircularBuffer (lock-free ring buffer)
  → AudioUnit render callback reads from ring buffer
  → BlackHole output → BlackHole input
  → AirType / WeChat reads from BlackHole
```

### Session Lifecycle

- `init_mic_redirect_device()`: Called when mic socket becomes enabled
- `release_mic_redirect_device()`: Called when mic socket becomes disabled (session end)
- Cleanup destroys aggregate device and restores all audio state
