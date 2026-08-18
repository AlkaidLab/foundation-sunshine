# DualSense Legacy Haptics Tuning

The authored haptics SDK remains device-independent. It analyzes PCM and
returns `AhAuthoredHapticFrame`; Sunshine owns the ERM renderer and the legacy
`RUMBLE_DATA` policy.

The renderer supports four profiles:

| Profile | Strength | Curve | Gate | Max output | High scale | Response | Body mix |
| --- | ---: | ---: | ---: | ---: | ---: | --- | ---: |
| quiet | 0.75 | 0.75 | 0.008 | 0.55 | 0.65 | smooth | 0.10 |
| balanced | 1.00 | 0.50 | 0.006 | 0.70 | 0.75 | balanced | 0.15 |
| strong | 1.10 | 0.40 | 0.004 | 0.82 | 0.85 | fast | 0.18 |
| custom | user supplied | user supplied | user supplied | user supplied | user supplied | user supplied | user supplied |

`ds5_config.json` v1 files containing the original five fields remain readable.
The next save writes schema 2 and adds `profile`, `max_output`, `high_scale`,
`response`, and `body_mix`. Invalid ranges, profile names, and response names
are rejected before a snapshot is published.

The mapping stages are:

```text
AhAuthoredHapticFrame -> two ERM energy lanes -> gate/curve/strength
  -> high-motor scale/body mix -> output ceilings -> response/slew limiting
  -> 16-bit RUMBLE_DATA
```

The 80 ms minimum active hold protects short pulses from clients that keep only
the newest queued rumble packet. Stream-end and watchdog paths bypass smoothing
and emit zero immediately. Presets are starting calibration values; manual
validation should compare quiet-band perceptibility against combat motor noise.
