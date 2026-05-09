# Voice Changer Reference Service

Reference implementation of the Sunshine voice-changer IPC backend.

> 中文版见 [`README_zh.md`](./README_zh.md).

## Quick start (passthrough smoke test)

```powershell
python voice_changer_server.py
```

Then in Sunshine config (Web UI → Audio/Video → Voice changer):

- Backend: **External service (UDP loopback)**
- Host: `127.0.0.1`
- Port: `9876`
- Timeout: `15` ms

The default `identity` backend echoes input PCM verbatim; useful to verify
end-to-end wiring before plugging in a real model.

## RVC (real voice conversion)

```powershell
# 1. install ML deps (~3 GB with torch+CUDA)
pip install -r requirements-rvc.txt

# 2. download required pretrained weights (~350 MB)
python download_models.py --pretrained

# 3. download or copy in .pth + .index voice model files
#    (see download_models.py --list, or use --url <hf-url>)

# 4. run with the rvc backend
python voice_changer_server.py --backend rvc --warmup `
    --opt model_path=models/voices/character.pth `
    --opt index_path=models/voices/character.index `
    --opt pitch_shift=12 `
    --opt index_rate=0.75
```

Full guide (Chinese): [`README_zh.md`](./README_zh.md).

## Wire protocol v1

See [`src/voice_changer/voice_changer_ipc.h`](../../src/voice_changer/voice_changer_ipc.h)
for the canonical definition.

| Offset | Size | Field |
|--------|------|-------|
| 0      | 4    | magic = `0x56434843` (`'VCHC'`) |
| 4      | 1    | version = 1 |
| 5      | 1    | msg_type (1=PROCESS_REQ, 2=PROCESS_RSP, 3=PING, 4=PONG) |
| 6      | 2    | flags |
| 8      | 4    | seq (echo on response) |
| 12     | 4    | sample_rate (Hz) |
| 16     | 2    | channels |
| 18     | 2    | sample_count (per channel) |
| 20     | 4    | reserved (0) |
| 24     | …    | int16 PCM payload |

For Sunshine 20 ms mic frames at 48 kHz mono, payload is 1920 bytes per
packet (1944 bytes including header), well within the IPv4 safe MTU.

## Adding a backend

Drop a module in `backends/<your_name>.py` exposing a `create(**opts)`
factory that returns an object with `name: str` and
`process(samples, sample_rate, channels) -> bytes`. Then register it in
`backends/__init__.py::REGISTRY`. Examples:

- `backends/identity.py` — minimal skeleton
- `backends/rvc.py` — full streaming wrapper around
  [fumiama/Retrieval-based-Voice-Conversion](https://github.com/fumiama/Retrieval-based-Voice-Conversion)

Other backends worth wrapping:
- [IAHispano/Applio](https://github.com/IAHispano/Applio) (modern RVC fork, MIT)
- [Plachtaa/seed-vc](https://github.com/Plachtaa/seed-vc) (zero-shot, no training)
- [w-okada/voice-changer](https://github.com/w-okada/voice-changer) (existing GUI)
- so-vits-svc 4.x / DDSP-SVC
- Plain DSP via `sox` / `librosa` (pitch shift, reverb, robot voice, …)

## Latency budget

The Sunshine client side enforces a per-frame timeout (default 15 ms) and
falls back to passthrough on miss. Keep `process()` well under that bound
or accept occasional dropouts. Loopback UDP RTT is < 1 ms; the rest is
your inference budget.

## Process management

This service does not daemonize. Recommended setups:

- Windows: `Start-Process -WindowStyle Hidden python voice_changer_server.py`
  from a startup script, or wrap with NSSM as a service.
- Linux/macOS: launchd / systemd user unit.

A future PR may add an opt-in helper-process supervisor inside Sunshine
itself (similar to how `vmouse` and `vdd` install/manage their drivers).
