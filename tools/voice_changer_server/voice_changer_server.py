#!/usr/bin/env python3
"""Voice-changer sidecar service for Sunshine.

Implements the wire protocol v1 documented in
``src/voice_changer/voice_changer_ipc.h``. Multiple inference backends are
pluggable via ``--backend``; ship-default is ``identity`` (passthrough) so
the sidecar always runs without ML deps installed.

Usage::

    # smoke test (passthrough)
    python voice_changer_server.py

    # RVC inference
    python voice_changer_server.py --backend rvc \\
        --opt model_path=models/voices/character.pth \\
        --opt index_path=models/voices/character.index \\
        --opt pitch_shift=12

Each ``--opt key=value`` pair is forwarded to the backend factory. Keys are
backend-specific; see ``backends/<name>.py``.

Protocol summary (24-byte header + int16 PCM payload):
    magic=0x56434843 'VCHC', version=1, types: 1 PROCESS_REQ / 2 PROCESS_RSP
    / 3 PING / 4 PONG.
"""

from __future__ import annotations

import argparse
import logging
import socket
import struct
import sys
from dataclasses import dataclass

from backends import load as load_backend

LOG = logging.getLogger("voice_changer_server")

# Wire constants — keep in sync with src/voice_changer/voice_changer_ipc.h.
MAGIC = 0x56434843  # 'VCHC'
VERSION = 1
MSG_PROCESS_REQ = 1
MSG_PROCESS_RSP = 2
MSG_PING = 3
MSG_PONG = 4
HEADER_SIZE = 24
HEADER_FMT = "<IBBHIIHHI"  # magic, ver, type, flags, seq, sr, ch, samp, reserved
assert struct.calcsize(HEADER_FMT) == HEADER_SIZE


@dataclass
class Frame:
    msg_type: int
    flags: int
    seq: int
    sample_rate: int
    channels: int
    sample_count: int
    payload: bytes


def decode(buf: bytes) -> Frame | None:
    if len(buf) < HEADER_SIZE:
        return None
    magic, ver, msg, flags, seq, sr, ch, sc, _ = struct.unpack_from(HEADER_FMT, buf, 0)
    if magic != MAGIC or ver != VERSION:
        return None
    payload = buf[HEADER_SIZE:HEADER_SIZE + sc * ch * 2]
    return Frame(msg, flags, seq, sr, ch, sc, payload)


def encode(frame: Frame) -> bytes:
    return (
        struct.pack(
            HEADER_FMT,
            MAGIC,
            VERSION,
            frame.msg_type,
            frame.flags,
            frame.seq,
            frame.sample_rate,
            frame.channels,
            frame.sample_count,
            0,
        )
        + frame.payload
    )


def serve(host: str, port: int, backend) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((host, port))
    LOG.info("voice_changer_server listening on udp://%s:%d backend=%s",
             host, port, backend.name)

    frames_processed = 0
    last_log = 0
    fallbacks = 0

    while True:
        try:
            data, addr = sock.recvfrom(8192)
        except KeyboardInterrupt:
            LOG.info("shutting down")
            return
        except OSError as e:
            LOG.warning("recvfrom failed: %s", e)
            continue

        frame = decode(data)
        if frame is None:
            LOG.debug("dropping malformed packet from %s (len=%d)", addr, len(data))
            continue

        if frame.msg_type == MSG_PING:
            reply = encode(Frame(MSG_PONG, 0, frame.seq, frame.sample_rate,
                                 frame.channels, 0, b""))
            sock.sendto(reply, addr)
            continue

        if frame.msg_type != MSG_PROCESS_REQ:
            continue

        try:
            out_payload = backend.process(frame.payload, frame.sample_rate, frame.channels)
        except Exception as e:
            fallbacks += 1
            if fallbacks % 100 == 1:
                LOG.exception("backend.process failed (fallback #%d), echoing input: %s",
                              fallbacks, e)
            out_payload = frame.payload

        if len(out_payload) != len(frame.payload):
            LOG.warning("backend returned %d bytes, expected %d; padding/truncating",
                        len(out_payload), len(frame.payload))
            if len(out_payload) > len(frame.payload):
                out_payload = out_payload[:len(frame.payload)]
            else:
                out_payload = out_payload + b"\x00" * (len(frame.payload) - len(out_payload))

        reply = encode(Frame(
            MSG_PROCESS_RSP, 0, frame.seq, frame.sample_rate,
            frame.channels, frame.sample_count, out_payload,
        ))
        sock.sendto(reply, addr)

        frames_processed += 1
        if frames_processed - last_log >= 500:
            LOG.info("processed %d frames (fallbacks=%d)", frames_processed, fallbacks)
            last_log = frames_processed


def parse_opts(opts: list[str]) -> dict[str, object]:
    """Parse ``--opt key=value`` pairs. Numeric values are auto-converted."""
    out: dict[str, object] = {}
    for kv in opts:
        if "=" not in kv:
            raise SystemExit(f"--opt must be key=value, got: {kv}")
        k, _, v = kv.partition("=")
        try:
            out[k] = int(v)
            continue
        except ValueError:
            pass
        try:
            out[k] = float(v)
            continue
        except ValueError:
            pass
        out[k] = v
    return out


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=9876)
    p.add_argument("--backend", default="identity",
                   help="backend name (identity, rvc); see backends/ directory")
    p.add_argument("--opt", action="append", default=[],
                   metavar="KEY=VALUE",
                   help="backend-specific option (repeatable)")
    p.add_argument("--warmup", action="store_true",
                   help="invoke backend.warmup() after init (preloads model/CUDA)")
    p.add_argument("--log-level", default="INFO")
    args = p.parse_args(argv)

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    opts = parse_opts(args.opt)
    LOG.info("loading backend '%s' opts=%s", args.backend, opts)
    try:
        backend = load_backend(args.backend, **opts)
    except Exception as e:
        LOG.error("failed to load backend '%s': %s", args.backend, e)
        return 2

    if args.warmup and hasattr(backend, "warmup"):
        try:
            backend.warmup()
        except Exception as e:
            LOG.warning("warmup failed: %s", e)

    serve(args.host, args.port, backend)
    return 0


if __name__ == "__main__":
    sys.exit(main())
