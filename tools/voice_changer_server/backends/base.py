"""Backend protocol shared by all voice-changer implementations.

PCM convention used everywhere:
- raw little-endian int16 bytes
- mono only at this layer (multi-channel handled by the IPC server)
- sample rate as advertised in the request header (Sunshine sends 48000)

`process` MUST return bytes of the same length as the input. If the backend
needs internal buffering / look-ahead (e.g. RVC chunking) it must pad/truncate
on the way out — the calling server treats any length mismatch as a hard error
and substitutes silence.
"""

from __future__ import annotations

from typing import Protocol, runtime_checkable


@runtime_checkable
class Backend(Protocol):
    name: str

    def process(self, samples: bytes, sample_rate: int, channels: int) -> bytes: ...

    # Optional:
    def reset(self) -> None: ...  # called when stream restarts; default no-op
    def warmup(self) -> None: ...  # called after init; default no-op
