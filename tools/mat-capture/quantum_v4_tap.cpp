#define NOMINMAX
#include "quantum_v4_recorder.h"
#include <MinHook.h>
#include <cstring>
using Render4 = void(__fastcall *)(void *, uint32_t, void *, uint32_t, void *);
using Submit4 = HRESULT(__fastcall *)(void *, const void *, const void *, const float *, uint32_t, float);
using Extended4 = HRESULT(__fastcall *)(void *, const void *, const void *, const void *, const float *, uint32_t, float);
using Process4 = HRESULT(__fastcall *)(void *, uint32_t, uint32_t, BYTE *, uint32_t *);
using Reset4 = HRESULT(__fastcall *)(void *);
using Destroy4 = void(__fastcall *)(void *);
static Render4 originalRender4;
static Submit4 originalSubmit4;
static Extended4 originalExtended4;
static Process4 originalProcess4;
static Reset4 originalReset4;
static Destroy4 originalDestroy4;
static void *targets4[6] {};
static BYTE saved4[6][16] {};
static volatile LONG active4, running4;
static volatile LONG captureAdmissions4;
static void *volatile current4;
static HMODULE self4;
static bool created4;
static q4::Recorder recorder4;
static __declspec(thread) q4::Pass *pass4;
static q4::Ring *
currentRing4() { return static_cast<q4::Ring *>(InterlockedCompareExchangePointer(&current4, nullptr, nullptr)); }
static bool
admitted4(q4::Ring *ring, q4::Pass *context) { return ring && recorder4.healthy() && (!ring->stop || (context && context->valid)); }
static bool
binding4(void *renderer, uint32_t count, void *input, uint64_t &encoder, uint64_t &implementation, uint32_t &frames) {
  bool valid = false;
  __try {
    auto *wrapper = *reinterpret_cast<void **>(static_cast<BYTE *>(renderer) + 8);
    auto **table = *static_cast<void ***>(wrapper);
    if (count == 1 && input && table[2] == targets4[1] && table[3] == targets4[2] && table[5] == targets4[3] && table[6] == targets4[4]) {
      encoder = reinterpret_cast<uint64_t>(wrapper);
      implementation = *reinterpret_cast<uint64_t *>(static_cast<BYTE *>(wrapper) + 8);
      auto *connection = *static_cast<BYTE **>(input);
      memcpy(&frames, connection + 8, 4);
      valid = implementation != 0;
    }
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return valid;
}
static void __fastcall
HookRender4(void *renderer, uint32_t inputs, void *input, uint32_t outputs, void *output) {
  InterlockedIncrement(&active4);
  auto *ring = currentRing4();
  q4::Pass local {};
  auto *previous = pass4;
  // Admission increments BEFORE reading stop: the management thread can wait
  // for admitted passes before unpatching their remaining original calls.
  InterlockedIncrement(&captureAdmissions4);
  if (ring && !ring->stop) {
    InterlockedIncrement64(&ring->renderCalls);
    uint64_t encoder = 0, implementation = 0;
    uint32_t frames = 0;
    if (previous || !binding4(renderer, inputs, input, encoder, implementation, frames))
      recorder4.fail();
    else if (recorder4.begin(reinterpret_cast<uint64_t>(renderer), encoder, implementation, frames, local))
      pass4 = &local;
  }
  bool admitted = local.valid;
  if (!admitted) InterlockedDecrement(&captureAdmissions4);
  __try {
    originalRender4(renderer, inputs, input, outputs, output);
  }
  __finally {
    if (AbnormalTermination() && ring) recorder4.fail();
    if (local.valid) recorder4.end(local);
    if (admitted) InterlockedDecrement(&captureAdmissions4);
    pass4 = previous;
    InterlockedDecrement(&active4);
  }
}
static HRESULT __fastcall
HookSubmit4(void *encoder, const void *info, const void *properties, const float *pcm, uint32_t frames, float gain) {
  InterlockedIncrement(&active4);
  auto *ring = currentRing4();
  auto *context = pass4;
  bool captured = false;
  if (admitted4(ring, context)) {
    if (!context || context->encoder != reinterpret_cast<uint64_t>(encoder))
      recorder4.unbound();
    else
      captured = recorder4.before(*context, info, properties, pcm, frames, gain);
  }
  HRESULT result = E_UNEXPECTED;
  __try {
    result = originalSubmit4(encoder, info, properties, pcm, frames, gain);
  }
  __finally {
    bool abnormal = AbnormalTermination() != FALSE;
    if (abnormal && ring) recorder4.fail();
    if (context) recorder4.returned(*context, result, captured && !abnormal);
    InterlockedDecrement(&active4);
  }
  return result;
}
static HRESULT __fastcall
HookExtended4(void *encoder, const void *info, const void *properties, const void *metadata, const float *pcm, uint32_t frames, float gain) {
  InterlockedIncrement(&active4);
  auto *ring = currentRing4();
  if (admitted4(ring, pass4)) {
    InterlockedIncrement64(&ring->extendedCalls);
    recorder4.fail();
  }
  HRESULT result = E_UNEXPECTED;
  __try {
    result = originalExtended4(encoder, info, properties, metadata, pcm, frames, gain);
  }
  __finally {
    if (AbnormalTermination() && ring) recorder4.fail();
    InterlockedDecrement(&active4);
  }
  return result;
}
static HRESULT __fastcall
HookProcess4(void *encoder, uint32_t frames, uint32_t capacity, BYTE *output, uint32_t *produced) {
  InterlockedIncrement(&active4);
  HRESULT result = E_UNEXPECTED;
  auto *ring = currentRing4();
  auto *context = pass4;
  __try {
    result = originalProcess4(encoder, frames, capacity, output, produced);
  }
  __finally {
    if (AbnormalTermination()) {
      if (ring) recorder4.fail();
    }
    else if (admitted4(ring, context)) {
      if (!context || context->encoder != reinterpret_cast<uint64_t>(encoder))
        recorder4.unbound();
      else
        recorder4.processed(*context, frames, capacity, produced, result);
    }
    InterlockedDecrement(&active4);
  }
  return result;
}
static HRESULT __fastcall
HookReset4(void *encoder) {
  InterlockedIncrement(&active4);
  auto *ring = currentRing4();
  if (ring && !ring->stop) recorder4.retire(reinterpret_cast<uint64_t>(encoder), q4::Reset);
  HRESULT result = E_UNEXPECTED;
  __try {
    result = originalReset4(encoder);
  }
  __finally {
    if (AbnormalTermination() && ring) recorder4.fail();
    InterlockedDecrement(&active4);
  }
  return result;
}
static void __fastcall
HookDestroy4(void *encoder) {
  InterlockedIncrement(&active4);
  auto *ring = currentRing4();
  if (ring && !ring->stop) recorder4.retire(reinterpret_cast<uint64_t>(encoder), q4::Destroy);
  __try {
    originalDestroy4(encoder);
  }
  __finally {
    if (AbnormalTermination() && ring) recorder4.fail();
    InterlockedDecrement(&active4);
  }
}
extern "C" __declspec(dllexport) DWORD WINAPI
StartCapture(void *input) {
  const auto args = *static_cast<StartArgs *>(input);
  if (InterlockedCompareExchange(&running4, 1, 0)) {
    CloseHandle(args.mapping);
    CloseHandle(args.collector);
    return ERROR_BUSY;
  }
  auto *ring = static_cast<q4::Ring *>(MapViewOfFile(args.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(q4::Ring)));
  CloseHandle(args.mapping);
  if (!ring) {
    CloseHandle(args.collector);
    InterlockedExchange(&running4, 0);
    return ERROR_INVALID_HANDLE;
  }
  DWORD error = 0;
  bool recorderStarted = false;
  wchar_t name[MAX_PATH] {};
  GetModuleFileNameW(nullptr, name, MAX_PATH);
  auto *file = wcsrchr(name, L'\\');
  file = file ? file + 1 : name;
  auto *base = reinterpret_cast<BYTE *>(GetModuleHandleW(L"audioeng.dll"));
  constexpr DWORD rvas[] = { 0xfa3b0, 0x2fd90, 0x45be0, 0x31e00, 0x74b50, 0x733f8 };
  const BYTE expected[][8] = { { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x55 }, { 0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x49, 0x08 },
    { 0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x49, 0x08 }, { 0x48, 0x83, 0xec, 0x38, 0x48, 0x8b, 0x49, 0x08 },
    { 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x49, 0x08 }, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b } };
  if (_wcsicmp(file, L"audiodg.exe") || !base || ring->magic != kMagic || ring->version != q4::version || !args.collector) error = ERROR_BAD_ENVIRONMENT;
  if (!error) {
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kTimestamp || nt->OptionalHeader.SizeOfImage != kImageSize) error = ERROR_REVISION_MISMATCH;
    for (unsigned i = 0; i < 6 && !error; ++i)
      if (memcmp(base + rvas[i], expected[i], 8)) error = ERROR_REVISION_MISMATCH;
  }
  if (!error && !created4) {
    HMODULE pinned {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(self4), &pinned)) error = GetLastError();
    auto status = error ? MH_ERROR_NOT_INITIALIZED : MH_Initialize();
    if (!error && status != MH_OK) error = 0x5000 + status;
    void *hooks[] = { reinterpret_cast<void *>(HookRender4), reinterpret_cast<void *>(HookSubmit4), reinterpret_cast<void *>(HookExtended4),
      reinterpret_cast<void *>(HookProcess4), reinterpret_cast<void *>(HookReset4), reinterpret_cast<void *>(HookDestroy4) };
    void **originals[] = { reinterpret_cast<void **>(&originalRender4), reinterpret_cast<void **>(&originalSubmit4), reinterpret_cast<void **>(&originalExtended4),
      reinterpret_cast<void **>(&originalProcess4), reinterpret_cast<void **>(&originalReset4), reinterpret_cast<void **>(&originalDestroy4) };
    for (unsigned i = 0; i < 6 && !error; ++i) {
      targets4[i] = base + rvas[i];
      memcpy(saved4[i], targets4[i], 16);
      status = MH_CreateHook(targets4[i], hooks[i], originals[i]);
      if (status != MH_OK) error = 0x5100 + i * 0x100 + status;
    }
    if (!error) created4 = true;
  }
  if (!error) {
    recorder4.start(ring);
    recorderStarted = true;
    ring->enginePid = GetCurrentProcessId();
    ring->backend = q4::backend;
    ring->encoderRva = rvas[1];
    InterlockedExchangePointer(&current4, ring);
    for (auto *target : targets4) {
      auto status = MH_QueueEnableHook(target);
      if (status != MH_OK && !error) error = 0x5800 + status;
    }
    if (!error) {
      auto status = MH_ApplyQueued();
      if (status != MH_OK) error = 0x5900 + status;
    }
    if (!error) {
      InterlockedExchange(&ring->encoderReady, 1);
      InterlockedExchange(&ring->status, 1);
    }
    while (!error && !ring->stop && WaitForSingleObject(args.collector, 20) == WAIT_TIMEOUT && static_cast<LONG64>(GetTickCount64()) - InterlockedCompareExchange64(&ring->heartbeat, 0, 0) < 5000) {}
  }
  InterlockedExchange(&ring->stop, 1);
  while (InterlockedCompareExchange(&captureAdmissions4, 0, 0)) Sleep(1);
  if (created4) {
    auto status = MH_DisableHook(MH_ALL_HOOKS);
    if (status != MH_OK && !error) error = 0x5a00 + status;
  }
  InterlockedExchangePointer(&current4, nullptr);
  while (InterlockedCompareExchange(&active4, 0, 0)) Sleep(1);
  if (recorderStarted) recorder4.finish();
  for (unsigned i = 0; i < 6; ++i)
    if (targets4[i] && memcmp(targets4[i], saved4[i], 16)) error = ERROR_WRITE_FAULT;
  if (!error && (ring->invalid || ring->droppedFull || ring->droppedBusy || ring->unboundAfterArming)) error = ERROR_INVALID_DATA;
  CloseHandle(args.collector);
  ring->error = error;
  InterlockedExchange(&ring->status, error ? -1 : 2);
  UnmapViewOfFile(ring);
  InterlockedExchange(&running4, 0);
  return error;
}
BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, void *) {
  if (reason == DLL_PROCESS_ATTACH) {
    self4 = instance;
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}
