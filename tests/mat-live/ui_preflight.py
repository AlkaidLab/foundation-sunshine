"""Real Web UI routes on the caller's isolated host. No native capture source."""
import ctypes
from ctypes import wintypes
import json
import urllib.request


def verify_ui(opener, port, config, config_path):
    def request(route, body=None):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(f"https://127.0.0.1:{port+1}/api/{route}", data=data,
                                     headers={"Content-Type": "application/json"})
        with opener.open(req, timeout=5) as response:
            if route == "mat/status":
                assert response.headers.get("Cache-Control") == "no-store"
            return json.load(response)

    # Pending connection detects even a short-lived accidental status probe.
    class Overlapped(ctypes.Structure):
        _fields_ = [("internal", ctypes.c_size_t), ("internal_high", ctypes.c_size_t),
                    ("offset", wintypes.DWORD), ("offset_high", wintypes.DWORD), ("event", wintypes.HANDLE)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                        wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    kernel.CreateNamedPipeW.restype = wintypes.HANDLE
    kernel.CreateEventW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR]
    kernel.CreateEventW.restype = wintypes.HANDLE
    kernel.ConnectNamedPipe.argtypes = [wintypes.HANDLE, ctypes.POINTER(Overlapped)]
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.CancelIoEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(Overlapped)]
    kernel.GetOverlappedResult.argtypes = [wintypes.HANDLE, ctypes.POINTER(Overlapped), ctypes.POINTER(wintypes.DWORD), wintypes.BOOL]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    pipe = kernel.CreateNamedPipeW(r"\\.\pipe\SunshineMatLive-active", 3 | 0x40000000 | 0x80000, 0, 1, 64, 64, 0, None)
    if pipe == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    event = kernel.CreateEventW(None, True, False, None)
    overlapped = Overlapped(event=event)
    pending = False
    try:
        if not event:
            raise ctypes.WinError(ctypes.get_last_error())
        connected = kernel.ConnectNamedPipe(pipe, ctypes.byref(overlapped))
        pending = not connected and ctypes.get_last_error() == 997  # ERROR_IO_PENDING
        assert pending
        for _ in range(10):
            assert request("mat/status") == {"state": "unobserved", "last_error": ""}
        original = config_path.read_bytes()
        for invalid in ("invalid", "3"):
            assert request("config", {**config, "mat_live": invalid})["status"] == "false"
            assert config_path.read_bytes() == original
        for enabled in (False, True):
            settings = {**config}
            settings["mat_live"] = "true" if enabled else "false"
            settings.update(mat_live_pipe="retired", mat_live_producer="retired")
            assert request("config", settings)["status"] == "true"
            saved = request("config")
            assert saved["mat_live"] == settings["mat_live"]
            assert "mat_live_pipe" not in saved and "mat_live_producer" not in saved
            text = config_path.read_text(encoding="utf8")
            assert "mat_live_pipe" not in text and "mat_live_producer" not in text
            # Saved changes await restart; runtime observation is independent.
            assert request("mat/status") == {"state": "unobserved", "last_error": ""}
        assert kernel.WaitForSingleObject(event, 0) == 258  # no pipe connection
    finally:
        if pending:
            kernel.CancelIoEx(pipe, ctypes.byref(overlapped))
            count = wintypes.DWORD()
            kernel.GetOverlappedResult(pipe, ctypes.byref(overlapped), ctypes.byref(count), True)
        if event:
            kernel.CloseHandle(event)
        kernel.CloseHandle(pipe)
    return {"readOnlyPolls": 10, "invalidSwitchesRejected": 2, "switchRoundTrips": 2, "legacyParametersRemoved": True,
            "capturePipeOpened": False, "savedSettingsRequireRestart": True}
