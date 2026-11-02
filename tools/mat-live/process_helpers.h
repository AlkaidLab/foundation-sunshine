#pragma once
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <stdexcept>
namespace fs = std::filesystem;
static volatile LONG interrupted;
static BOOL WINAPI
Ctrl(DWORD) {
  InterlockedExchange(&interrupted, 1);
  return TRUE;
}
static void
Require(bool ok, const char *what) {
  if (!ok) {
    printf("ERROR %s win32=%lu\n", what, GetLastError());
    throw std::runtime_error(what);
  }
}
static LONG64
Read64(volatile LONG64 *p) { return InterlockedCompareExchange64(p, 0, 0); }
static uintptr_t
Module(DWORD pid, const wchar_t *name) {
  HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
  MODULEENTRY32W m {};
  m.dwSize = sizeof(m);
  uintptr_t base = 0;
  if (Module32FirstW(s, &m)) do {
      if (!_wcsicmp(m.szModule, name)) {
        base = reinterpret_cast<uintptr_t>(m.modBaseAddr);
        break;
      }
    } while (Module32NextW(s, &m));
  CloseHandle(s);
  return base;
}
static HANDLE
RemoteThread(HANDLE process, uintptr_t entry, void *arg) {
  HANDLE t = CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(entry), arg, 0, nullptr);
  Require(t != nullptr, "CreateRemoteThread");
  return t;
}
static void *
RemoteCopy(HANDLE process, const void *data, size_t bytes) {
  void *p = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  Require(p != nullptr, "VirtualAllocEx");
  if (!WriteProcessMemory(process, p, data, bytes, nullptr)) {
    const auto error = GetLastError();
    VirtualFreeEx(process, p, 0, MEM_RELEASE);
    SetLastError(error);
    Require(false, "WriteProcessMemory");
  }
  return p;
}
static uintptr_t
LoadTap(HANDLE process, DWORD pid, const fs::path &path) {
  uintptr_t base = Module(pid, path.filename().c_str());
  if (base) return base;
  auto loader = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
  HMODULE owner {};
  Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(loader), &owner) != FALSE,
    "Loader owner");
  wchar_t ownerPath[MAX_PATH] {};
  GetModuleFileNameW(owner, ownerPath, MAX_PATH);
  uintptr_t remoteOwner = Module(pid, fs::path(ownerPath).filename().c_str());
  Require(remoteOwner != 0, "Remote loader module");
  std::wstring value = path.wstring();
  void *arg = RemoteCopy(process, value.c_str(), (value.size() + 1) * sizeof(wchar_t));
  HANDLE t;
  try {
    t = RemoteThread(process, remoteOwner + reinterpret_cast<uintptr_t>(loader) - reinterpret_cast<uintptr_t>(owner), arg);
  }
  catch (...) {
    VirtualFreeEx(process, arg, 0, MEM_RELEASE);
    throw;
  }
  DWORD wait = WaitForSingleObject(t, 10000);
  DWORD loaderExit {};
  GetExitCodeThread(t, &loaderExit);
  CloseHandle(t);
  printf("LoadLibrary remoteThreadExit=0x%08lX\n", loaderExit);
  // Never free memory a timed-out loader might still be reading.
  Require(wait == WAIT_OBJECT_0, "LoadLibrary timeout (argument retained)");
  VirtualFreeEx(process, arg, 0, MEM_RELEASE);
  base = Module(pid, path.filename().c_str());
  Require(base != 0, "Loaded engine_tap module");
  return base;
}
