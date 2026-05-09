"""Identity passthrough backend.

Echoes input verbatim. Used as the default backend so the sidecar is always
runnable without ML deps installed, and as a smoke-test target for the IPC
protocol.
"""

from __future__ import annotations


class IdentityBackend:
    name = "identity"

    def process(self, samples: bytes, sample_rate: int, channels: int) -> bytes:
        return samples

    def reset(self) -> None:
        pass

    def warmup(self) -> None:
        pass


def create(**_opts: object) -> IdentityBackend:
    return IdentityBackend()
