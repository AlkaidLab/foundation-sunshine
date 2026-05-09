"""RVC (Retrieval-based Voice Conversion) backend.

Wraps `fumiama/Retrieval-based-Voice-Conversion` (the inference-only fork of
RVC-Project) via a streaming overlap-add buffer. Suitable for sidecar use:
file-based model + index loaded once, then per-frame conversion.

Lazy-imports `rvc` so the sidecar still starts (with the identity backend) on
machines that haven't installed the RVC dependency stack.

Required deps (install with `pip install -r requirements-rvc.txt`):
    rvc>=0.1.6  # fumiama
    torch
    librosa
    numpy
    soundfile
    fairseq

Required asset files:
    .pth  voice model (per-character; from HuggingFace, AI Hub etc.)
    .index  faiss index file paired with the model
    plus pretrained weights auto-managed by `download_models.py`:
        - hubert_base.pt
        - rmvpe.pt
"""

from __future__ import annotations

import logging
import os
import tempfile
from pathlib import Path
from typing import Any

LOG = logging.getLogger(__name__)


# Sunshine's mic stream is fixed at 48 kHz mono int16. Internal RVC sample rate
# depends on the model (40 / 48 / 32 kHz); we resample on the way in/out.
SUNSHINE_SR = 48000


class RvcBackend:
    """Streaming RVC voice converter.

    Loads model lazily on first use so import failures only affect the RVC
    backend, not the whole sidecar.
    """

    name = "rvc"

    def __init__(
        self,
        model_path: str,
        index_path: str | None = None,
        pitch_shift: int = 0,
        index_rate: float = 0.75,
        f0_method: str = "rmvpe",
        protect: float = 0.33,
        filter_radius: int = 3,
        rms_mix_rate: float = 0.25,
        chunk_ms: int = 320,
        overlap_ms: int = 80,
        device: str = "auto",
        speaker_id: int = 0,
    ) -> None:
        self.model_path = str(Path(model_path).resolve())
        self.index_path = str(Path(index_path).resolve()) if index_path else ""
        self.pitch_shift = pitch_shift
        self.index_rate = index_rate
        self.f0_method = f0_method
        self.protect = protect
        self.filter_radius = filter_radius
        self.rms_mix_rate = rms_mix_rate
        self.speaker_id = speaker_id
        self.device = device

        self._vc: Any = None
        self._loaded = False
        # numpy / streaming imported lazily so the identity backend remains
        # usable on machines without scientific Python installed.
        try:
            import numpy  # noqa: F401
            from .streaming import OverlapBuffer
        except ImportError as e:
            raise RuntimeError(
                "rvc backend requires numpy + librosa + soundfile + rvc. "
                "Run: pip install -r requirements-rvc.txt"
            ) from e
        self._np = numpy
        self._buffer = OverlapBuffer(
            sample_rate=SUNSHINE_SR,
            chunk_ms=chunk_ms,
            overlap_ms=overlap_ms,
            infer=self._infer_chunk,
        )

    # ---- Backend protocol ----

    def reset(self) -> None:
        self._buffer.reset()

    def warmup(self) -> None:
        self._lazy_load()
        # run a silent chunk through to warm CUDA caches
        try:
            silence = self._np.zeros(int(SUNSHINE_SR * 0.32), dtype=self._np.float32)
            _ = self._infer_chunk(silence, SUNSHINE_SR)
            LOG.info("RVC warmup ok")
        except Exception as e:
            LOG.warning("RVC warmup failed (will fall back to passthrough): %s", e)

    def process(self, samples: bytes, sample_rate: int, channels: int) -> bytes:
        if channels != 1:
            # Sunshine sends mono; refuse to surprise users with a downmix.
            raise ValueError(f"RVC backend expects mono, got channels={channels}")
        if sample_rate != SUNSHINE_SR:
            # Could resample but Sunshine's contract is 48k; warn loudly once.
            LOG.warning("unexpected sample_rate=%d (expected %d)",
                        sample_rate, SUNSHINE_SR)
            return samples  # passthrough rather than guess

        in_arr = _bytes_to_float(samples, self._np)
        n = len(in_arr)
        self._buffer.push(in_arr)
        out = self._buffer.pop(n)
        if out is None:
            # Not enough data accumulated yet; emit silence to keep the stream
            # synchronous. The first ~chunk_ms of audio will be silent — this
            # is the latency cost of overlap-add streaming.
            out = self._np.zeros(n, dtype=self._np.float32)
        return _float_to_bytes(out, self._np)

    # ---- internals ----

    def _lazy_load(self) -> None:
        if self._loaded:
            return
        try:
            from rvc.modules.vc.modules import VC  # type: ignore
        except ImportError as e:
            raise RuntimeError(
                "rvc package not installed. "
                "Run: pip install -r requirements-rvc.txt"
            ) from e

        self._vc = VC()
        self._vc.get_vc(self.model_path)
        self._loaded = True
        LOG.info("RVC model loaded: %s", self.model_path)
        if self.index_path:
            LOG.info("RVC index: %s", self.index_path)

    def _infer_chunk(self, chunk_f32, sr: int):
        """Run RVC on one ~320ms chunk. Returns float32 mono of same length."""
        self._lazy_load()
        np = self._np
        # fumiama's vc_inference is file-based; round-trip via tempfile.
        # Slow-ish (~1ms overhead) but avoids depending on private internals.
        try:
            import soundfile as sf  # type: ignore
        except ImportError as e:
            raise RuntimeError("soundfile not installed (pip install soundfile)") from e

        with tempfile.TemporaryDirectory() as td:
            in_path = os.path.join(td, "in.wav")
            # write at sunshine SR; RVC pipeline does its own internal resampling
            int16 = np.clip(chunk_f32 * 32767.0, -32768, 32767).astype(np.int16)
            sf.write(in_path, int16, sr, subtype="PCM_16")

            try:
                # try the high-level API first
                result = self._vc.vc_inference(
                    sid=self.speaker_id,
                    input_audio_path=in_path,
                    f0_up_key=self.pitch_shift,
                    f0_method=self.f0_method,
                    file_index=self.index_path or "",
                    index_rate=self.index_rate,
                    filter_radius=self.filter_radius,
                    resample_sr=sr,  # ask RVC to resample back to sr for us
                    rms_mix_rate=self.rms_mix_rate,
                    protect=self.protect,
                )
            except TypeError:
                # API drift fallback — retry with positional/keyword variants
                result = self._vc.vc_inference(
                    self.speaker_id,
                    in_path,
                    self.pitch_shift,
                    None,  # f0_file
                    self.f0_method,
                    self.index_path or "",
                    self.index_rate,
                    self.filter_radius,
                    sr,
                    self.rms_mix_rate,
                    self.protect,
                )

        # Result shape varies by version: (sr, audio_int16) or audio_int16 alone.
        if isinstance(result, tuple) and len(result) == 2:
            out_sr, out_audio = result
        else:
            out_sr, out_audio = sr, result

        out_audio = np.asarray(out_audio)
        if out_audio.dtype == np.int16:
            out_f32 = out_audio.astype(np.float32) / 32768.0
        else:
            out_f32 = out_audio.astype(np.float32)

        if out_sr != sr:
            out_f32 = _resample(out_f32, out_sr, sr)

        # ensure exact length match for OverlapBuffer
        target = len(chunk_f32)
        if len(out_f32) < target:
            out_f32 = np.concatenate([out_f32, np.zeros(target - len(out_f32), dtype=np.float32)])
        elif len(out_f32) > target:
            out_f32 = out_f32[:target]
        return out_f32


def create(**opts: Any) -> RvcBackend:
    if "model_path" not in opts or not opts["model_path"]:
        raise ValueError("rvc backend requires --model <path-to.pth>")
    return RvcBackend(**opts)


# ---- helpers ----


def _bytes_to_float(buf: bytes, np):
    arr = np.frombuffer(buf, dtype=np.int16).astype(np.float32) / 32768.0
    return arr


def _float_to_bytes(arr, np) -> bytes:
    clipped = np.clip(arr * 32767.0, -32768, 32767).astype(np.int16)
    return clipped.tobytes()


def _resample(audio, src_sr: int, dst_sr: int):
    if src_sr == dst_sr:
        return audio
    try:
        import librosa  # type: ignore
        return librosa.resample(audio, orig_sr=src_sr, target_sr=dst_sr)
    except ImportError:
        # crude linear resample fallback (lossy but no extra deps)
        import numpy as np
        ratio = dst_sr / src_sr
        new_len = int(len(audio) * ratio)
        x_old = np.linspace(0, 1, len(audio), endpoint=False, dtype=np.float32)
        x_new = np.linspace(0, 1, new_len, endpoint=False, dtype=np.float32)
        return np.interp(x_new, x_old, audio).astype(np.float32)
