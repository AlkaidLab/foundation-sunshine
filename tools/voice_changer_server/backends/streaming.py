"""Streaming chunk buffer with overlap-add cross-fading.

Voice-conversion models like RVC, SEED-VC, so-vits-svc are *not* designed
for per-frame inference. They need a context window of ~200-500ms for stable
F0 / embedding extraction. Calling them on every 20ms frame produces
boundary artifacts and ruins quality.

`OverlapBuffer` collects incoming small frames, runs the model once per
"chunk" (e.g. 320 ms), then crossfades adjacent chunks to mask seams. The
caller pulls fixed-size output frames matching the input frame size.

Latency footprint added by this layer: ~chunk_ms / 2 (output is delayed by
half a chunk to allow the next chunk to crossfade in).
"""

from __future__ import annotations

from collections import deque
from typing import Callable

import numpy as np

# Inference callable: (float32 mono, sample_rate) -> float32 mono of same length.
InferFn = Callable[[np.ndarray, int], np.ndarray]


class OverlapBuffer:
    def __init__(
        self,
        sample_rate: int,
        chunk_ms: int = 320,
        overlap_ms: int = 80,
        infer: InferFn | None = None,
    ) -> None:
        if overlap_ms * 2 >= chunk_ms:
            raise ValueError("overlap must be < chunk/2")
        self.sr = sample_rate
        self.chunk = int(sample_rate * chunk_ms / 1000)
        self.overlap = int(sample_rate * overlap_ms / 1000)
        self.hop = self.chunk - self.overlap  # samples advanced per chunk
        self.infer = infer
        self._in_buf: deque[np.ndarray] = deque()
        self._in_count = 0
        self._out_queue: deque[np.ndarray] = deque()
        self._tail: np.ndarray | None = None  # crossfade tail of previous chunk
        # equal-power crossfade window
        n = self.overlap
        t = np.linspace(0.0, 1.0, n, endpoint=False, dtype=np.float32)
        self._fade_in = np.sin(0.5 * np.pi * t)
        self._fade_out = np.cos(0.5 * np.pi * t)

    def reset(self) -> None:
        self._in_buf.clear()
        self._in_count = 0
        self._out_queue.clear()
        self._tail = None

    def push(self, frame: np.ndarray) -> None:
        """Feed an input frame (float32 mono)."""
        self._in_buf.append(frame)
        self._in_count += len(frame)
        while self._in_count >= self.chunk:
            self._consume_chunk()

    def pop(self, n: int) -> np.ndarray | None:
        """Return n samples of converted output, or None if not enough buffered yet."""
        if sum(len(x) for x in self._out_queue) < n:
            return None
        out = np.empty(n, dtype=np.float32)
        filled = 0
        while filled < n:
            head = self._out_queue[0]
            take = min(len(head), n - filled)
            out[filled:filled + take] = head[:take]
            filled += take
            if take == len(head):
                self._out_queue.popleft()
            else:
                self._out_queue[0] = head[take:]
        return out

    # --- internals ---

    def _consume_chunk(self) -> None:
        chunk = self._take_input(self.chunk)
        # advance input cursor by hop (keep overlap for next chunk)
        carry = chunk[self.hop:].copy()
        self._in_buf.appendleft(carry)
        self._in_count = sum(len(x) for x in self._in_buf)

        if self.infer is None:
            converted = chunk
        else:
            try:
                converted = self.infer(chunk, self.sr)
                if converted.shape != chunk.shape:
                    # caller must guarantee same length; pad/truncate defensively
                    if len(converted) > len(chunk):
                        converted = converted[:len(chunk)]
                    else:
                        pad = np.zeros(len(chunk) - len(converted), dtype=np.float32)
                        converted = np.concatenate([converted, pad])
            except Exception:
                # fall back to identity for this chunk; let server log
                converted = chunk

        # crossfade with previous tail
        if self._tail is not None:
            head = converted[:self.overlap]
            converted = converted.copy()
            converted[:self.overlap] = head * self._fade_in + self._tail * self._fade_out

        # split: emit (chunk - overlap) samples now, hold last `overlap` for next crossfade
        emit_len = self.chunk - self.overlap
        emit = converted[:emit_len]
        self._tail = converted[emit_len:].copy()
        self._out_queue.append(emit)

    def _take_input(self, n: int) -> np.ndarray:
        out = np.empty(n, dtype=np.float32)
        filled = 0
        while filled < n:
            head = self._in_buf.popleft()
            take = min(len(head), n - filled)
            out[filled:filled + take] = head[:take]
            filled += take
            if take < len(head):
                self._in_buf.appendleft(head[take:])
        self._in_count -= n
        return out
