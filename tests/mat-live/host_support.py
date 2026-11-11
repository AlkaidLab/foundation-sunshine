"""Isolated full Sunshine/NVHTTP/RTSP/Moonlight session, no media decoder.

Preprovisions an ephemeral paired certificate ONLY in this test's state file.
Never changes the installed host, Windows trust, drivers, or audio endpoints.
Timeouts report PIDs; no kill/restart fallback. WM_ENDSESSION targets our own
hidden Sunshine window and runs its ordinary shutdown path.
"""
import argparse
import ctypes
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import time
import urllib.parse
import urllib.request
import uuid
import winreg
import xml.etree.ElementTree as ET
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

def cert_pair(directory, name):
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(subject).issuer_name(subject)
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now-datetime.timedelta(days=1)).not_valid_after(now+datetime.timedelta(days=2))
            .sign(key, hashes.SHA256()))
    pem = cert.public_bytes(serialization.Encoding.PEM)
    (directory / (name+".pem")).write_bytes(pem)
    (directory / (name+".key")).write_bytes(key.private_bytes(serialization.Encoding.PEM,
        serialization.PrivateFormat.TraditionalOpenSSL,serialization.NoEncryption()))
    return pem.decode("ascii")

def check_bridge_absent():
    # Production startup cleans stale bridge values; require that there are none.
    for root in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
        try:
            key=winreg.OpenKey(root,r"SOFTWARE\Khronos\Vulkan\ImplicitLayers",0,winreg.KEY_READ|winreg.KEY_WOW64_64KEY)
        except FileNotFoundError:
            continue
        with key:
            for i in range(winreg.QueryInfoKey(key)[1]):
                name,_,_=winreg.EnumValue(key,i)
                if Path(name).name.lower()=="vklayer_zako_virtual_hdr.json":
                    raise RuntimeError("Existing Vulkan bridge registration: do not start isolated host")

def stop_own_host(process):
    if process.poll() is not None:
        return
    user=ctypes.WinDLL("user32",use_last_error=True)
    user.GetWindowThreadProcessId.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong)]
    user.GetClassNameW.argtypes=[ctypes.c_void_p,ctypes.c_wchar_p,ctypes.c_int]
    user.PostMessageW.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_size_t,ctypes.c_ssize_t]
    found=[]
    callback_type=ctypes.WINFUNCTYPE(ctypes.c_int,ctypes.c_void_p,ctypes.c_ssize_t)
    @callback_type
    def visit(hwnd,param):
        pid=ctypes.c_ulong();user.GetWindowThreadProcessId(hwnd,ctypes.byref(pid))
        if pid.value==process.pid:
            name=ctypes.create_unicode_buffer(256);user.GetClassNameW(hwnd,name,256)
            if name.value=="SunshineSessionMonitorClass":
                found.append(hwnd)
        return 1
    user.EnumWindows(visit,0)
    if len(found)!=1:
        raise RuntimeError(f"Own host PID {process.pid} has no unique shutdown window; left running")
    if not user.PostMessageW(found[0],0x16,1,0): # WM_ENDSESSION to our own process only
        raise ctypes.WinError(ctypes.get_last_error())
    process.wait(timeout=25)
