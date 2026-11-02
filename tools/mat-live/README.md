# Live MAT publisher support

`publisher.h` owns the bounded local pipe, native-source lease, fresh PLAY cutoff,
cycle delivery and bounded EOS acknowledgement. `process_helpers.h` contains only
the explicit process operations used by the capture launcher. There is no file
input, automatic engine selection, privilege adjustment or disk capture path.

The launcher is built by [sonic-mat-live/build.ps1](../sonic-mat-live/build.ps1).
The normal host accepts only the configured native publisher and engine identity.
The private protocol is version 2; network transport is common-c `mat-live-v1`.

`live_check.cpp` and `live_check_transport.c` are a native-pipe loopback checker
with a mock consumer, using the actual common-c RTP/FEC and authenticated receive
path. They never open a playback device. `build-live-check.ps1` also builds/runs
the generated UDP/FEC regression in `tests/mat-live`.

Run the [software verifier](../../tests/mat-live/README.md) for current regression
coverage. Historical probes, phone experiments, finite replay and native reports
are preserved on [the archive branch](https://github.com/AlkaidLab/foundation-sunshine/tree/codex/mat-history-20261023).
