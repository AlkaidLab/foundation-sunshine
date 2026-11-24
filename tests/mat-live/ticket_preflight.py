"""Abandoned RTSP claims must expire without another API call triggering prune.

Only an isolated desktop launch ticket and incomplete RTSP messages are used.
There is no ANNOUNCE/PLAY, native source, media capture or device output.
"""
import os
import socket
import time
from urllib.parse import urlsplit


def verify_ticket_expiry(request, app_id, log_path):
    peers = []
    marker = "Expired 1 pending RTSP launch ticket(s)"
    def log():
        return log_path.read_text(encoding="utf8", errors="replace")
    _, launch = request("launch", {"appid": app_id, "rikey": os.urandom(16).hex(), "rikeyid": 1,
        "mode": "640x360x30", "sops": 0, "localAudioPlayMode": 0, "surroundAudioInfo": 196610,
        "useVdd": 0, "customScreenMode": 0, "touchKeyboard": 0, "hdrMode": 0, "corever": 0,
        "remoteControllersBitmap": 0, "gcmap": 0, "gcpersist": 0})
    try:
        url = urlsplit(launch.findtext("sessionUrl0"))
        assert url.scheme == "rtsp" and url.hostname == "127.0.0.1"
        # First four bytes acquire a plaintext ticket. Remaining bytes leave
        # parsing incomplete, so the claim survives until its handshake timeout.
        for n in range(2):
            previous = log().count("handle_read_plaintext(): Handle read of size:")
            peer = socket.create_connection((url.hostname, url.port), timeout=3)
            peers.append(peer)
            peer.sendall(b"DESCRIBE")
            deadline = time.monotonic() + .5
            while log().count("handle_read_plaintext(): Handle read of size:") <= previous:
                assert time.monotonic() < deadline, "RTSP claim not observed"
                time.sleep(.01)
            if n == 0:
                time.sleep(.5)  # Different claim deadlines, overlapping ownership.
        for n, peer in enumerate(peers):
            peer.settimeout(3)
            try:
                assert peer.recv(1) == b"", "Expected handshake timeout without ANNOUNCE"
            except ConnectionResetError:
                pass  # Windows may reset a socket closed with an incomplete message.
            assert log().count("RTSP initial handshake timeout for peer") >= n + 1
            assert marker not in log(), "Ticket pruned while still claimed or before renewed expiry"
        released = time.monotonic()
        # No NVHTTP/status request here: those can prune opportunistically and
        # hide a missing asynchronous timer re-arm.
        while marker not in log():
            assert time.monotonic() - released < 3, "Released ticket never pruned by its timer"
            time.sleep(.02)
        elapsed = time.monotonic() - released
        assert elapsed > .7, "Final release did not renew the pending deadline"
        return {"claims": 2, "announced": False, "timerPruned": True,
                "secondsAfterFinalClose": elapsed, "mediaStarted": False}
    finally:
        for peer in peers:
            peer.close()
