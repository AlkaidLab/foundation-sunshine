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
    pipe = kernel.CreateNamedPipeW(config["mat_live_pipe"], 3 | 0x40000000, 0, 1, 64, 64, 0, None)
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
        for missing in ("mat_live_pipe", "mat_live_producer"):
            assert request("config", {**config, missing: ""})["status"] == "false"
            assert config_path.read_bytes() == original
        for enabled in (False, True):
            settings = {**config}
            if not enabled:
                settings.update(mat_live_pipe="", mat_live_producer="")
            assert request("config", settings)["status"] == "true"
            saved = request("config")
            assert saved.get("mat_live_pipe", "") == settings["mat_live_pipe"]
            assert saved.get("mat_live_producer", "") == settings["mat_live_producer"]
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
    return {"readOnlyPolls": 10, "partialSavesRejected": 2, "pairedRoundTrips": 2,
            "capturePipeOpened": False, "savedSettingsRequireRestart": True}
