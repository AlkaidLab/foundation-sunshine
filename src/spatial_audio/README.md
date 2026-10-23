# Live MAT source boundary

`EncodedAudioSource` exposes owned whole carrier cycles with start/read/stop,
identity, format and a 48 kHz carrier clock. `NativeMatPipeSource` accepts only the
configured native publisher after image, process, pipe and descriptor checks.
The host preserves failures and releases handles on preparation failure, read
failure, EOS or cancellation. A generated test-source descriptor is rejected.

`object_input_protocol.h` preserves the bounded object IPC layout used by the
external capture/worker pair. `stream_output_protocol.h` defines its continuous
output ring; `mat_stream_receiver.cpp` is built separately as the dedicated native
worker DLL. It is not compiled into Sunshine's normal audio capture path.
`native_encoded_protocol.h` and `native_mat_assembler.h` retain the half-block
contract and exact full-cycle assembly used by that worker's publisher.

There is no offline reader, file replay source, legacy broker or fixture DLL in
this directory. See [host integration](../../docs/MAT-HOST.md),
[capture tool](../../tools/sonic-mat-live/README.md) and
[software tests](../../tests/mat-live/README.md).
