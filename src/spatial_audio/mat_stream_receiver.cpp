#define NOMINMAX
#include "stream_output_protocol.h"
#include "mat_compat.h"
#include <roapi.h>
#include <activation.h>
#include <winstring.h>
#include <wrl/client.h>
#include <bcrypt.h>
#include <cmath>
#include <cwchar>
#include <new>
using namespace spatial_stream;
using Microsoft::WRL::ComPtr;
static HMODULE self;
static volatile LONG running;
using Submit = HRESULT(__fastcall *)(void *, const void *, const void *, const float *, UINT32, float);
using Process = HRESULT(__fastcall *)(void *, UINT32, BYTE *, UINT32 *);
struct Properties {
  uint32_t id, frames, type, flags;
  float x, y, z, volume;
  BYTE extra[0xb4 - 32];
};
static_assert(sizeof(Properties) == 0xb4);
struct Session {
  spatial_stream::Args args {};
  spatial_stream::FakeConfig config {};
  bool fake = false, ro = false;
  HANDLE handles[5] {}, available {}, space {}, worker {};
  Shared *input {};
  Output *output {};
  Stats *out {};
  alignas(64) volatile LONG64 write = 0, read = 0;
  volatile LONG stop = 0;
  Slot slots[8];
  ComPtr<IActivationFactory> factory;
  ComPtr<IInspectable> object;
  ComPtr<IUnknown> encoder, mapper;
  HMODULE vendor {};
  void *target {};
  Submit submit {};
  Process process {};
  const Call *expected {};
  BYTE stream[24] {};
  const float *borrowed[64] {};
  Call recorded[64] {};
  BYTE scratch[request] {};
  BYTE transcript[payloadCapacity] {};
  uint32_t pending = 0;
  ULONGLONG began = 0;
  ~Session() {
    if (worker) CloseHandle(worker);
    if (available) CloseHandle(available);
    if (space) CloseHandle(space);
    if (input) UnmapViewOfFile(input);
    if (output) UnmapViewOfFile(output);
    for (auto h : handles)
      if (h) CloseHandle(h);
  }
};
static GUID
guid(const wchar_t *p) {
  GUID g {};
  CLSIDFromString(p, &g);
  return g;
}
static bool
stopped(Session &s) {
  if (get(s.output->cancel)) {
    fail(*s.input, cancelled);
    return true;
  }
  if (get(s.output->error)) {
    fail(*s.input, get(s.output->error));
    return true;
  }
  if (get(s.input->common.fault)) return true;
  if (get(s.input->common.cancel) || WaitForSingleObject(s.args.cancel, 0) == WAIT_OBJECT_0) {
    fail(*s.input, cancelled);
    return true;
  }
  if (get(s.stop)) return true;
  auto owner = WaitForSingleObject(s.args.owner, 0);
  if (owner != WAIT_TIMEOUT) {
    fail(*s.input, owner == WAIT_OBJECT_0 ? producerExited : badContract);
    return true;
  }
  if (qpc() - get(s.input->writer.heartbeat) > hz()) {
    fail(*s.input, producerStale);
    return true;
  }
  if (s.args.maxSeconds && GetTickCount64() - s.began > s.args.maxSeconds * 1000ULL) {
    fail(*s.input, timeout);
    return true;
  }
  return false;
}
static bool
samePath(HMODULE module, const wchar_t *file) {
  wchar_t system[MAX_PATH] {}, expected[MAX_PATH] {}, actual[MAX_PATH] {};
  if (!GetSystemDirectoryW(system, MAX_PATH) || swprintf_s(expected, L"%s\\%s", system, file) < 0 || !GetModuleFileNameW(module, actual, MAX_PATH)) return false;
  return _wcsicmp(expected, actual) == 0;
}
static DWORD
hostGuard(BYTE **audio) {
  if (!samePath(nullptr, L"audiodg.exe")) return ERROR_BAD_ENVIRONMENT;
  *audio = reinterpret_cast<BYTE *>(GetModuleHandleW(L"audioeng.dll"));
  if (!*audio || !samePath(reinterpret_cast<HMODULE>(*audio), L"audioeng.dll")) return ERROR_BAD_ENVIRONMENT;
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(*audio);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return ERROR_REVISION_MISMATCH;
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(*audio + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp != 0x0627570d || nt->OptionalHeader.SizeOfImage != 0x1ef000) return ERROR_REVISION_MISMATCH;
  return 0;
}
static bool
fileHash(const wchar_t *path, const char *expected) {
  HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  BCRYPT_ALG_HANDLE algorithm {};
  BCRYPT_HASH_HANDLE hash {};
  BYTE *memory {};
  bool okay = false;
  DWORD objectBytes = 0, n = 0;
  LARGE_INTEGER size {};
  if (!GetFileSizeEx(f, &size) || size.QuadPart < 0 || size.QuadPart > 64 * 1024 * 1024) goto done;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 || BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes), 4, &n, 0) < 0) goto done;
  memory = static_cast<BYTE *>(HeapAlloc(GetProcessHeap(), 0, objectBytes));
  if (!memory || BCryptCreateHash(algorithm, &hash, memory, objectBytes, nullptr, 0, 0) < 0) goto done;
  {
    BYTE buffer[65536], digest[32];
    for (;;) {
      if (!ReadFile(f, buffer, sizeof(buffer), &n, nullptr)) goto done;
      if (!n) break;
      if (BCryptHashData(hash, buffer, n, 0) < 0) goto done;
    }
    if (BCryptFinishHash(hash, digest, 32, 0) < 0) goto done;
    okay = true;
    for (unsigned i = 0; i < 32; ++i) {
      auto hex = [](char c) { return c >= 'a' ? c - 'a' + 10 : c - '0'; };
      if (digest[i] != (hex(expected[i * 2]) * 16 + hex(expected[i * 2 + 1]))) okay = false;
    }
  }
done:
  if (hash) BCryptDestroyHash(hash);
  if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
  if (memory) HeapFree(GetProcessHeap(), 0, memory);
  CloseHandle(f);
  return okay;
}
static HRESULT
nativeOpen(Session &s, BYTE *audio) {
  // The publisher resolves the installed package; only its shape and the
  // pinned DLL contents are trusted here.
  const wchar_t *folder = s.args.vendorDirectory;
  if (!mat_compat::dolby_directory_shape(folder)) return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
  wchar_t enc[MAX_PATH] {}, engine[MAX_PATH] {}, audioPath[MAX_PATH] {};
  if (swprintf_s(enc, L"%sDolbyMatEnc.dll", folder) < 0 || swprintf_s(engine, L"%sDolbyMatEngine.dll", folder) < 0) return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
  if (GetFileAttributesW(enc) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(engine) == INVALID_FILE_ATTRIBUTES) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
  GetModuleFileNameW(reinterpret_cast<HMODULE>(audio), audioPath, MAX_PATH);
  bool pinned = false;
  for (auto expected : mat_compat::audioeng_sha256)
    pinned = pinned || fileHash(audioPath, expected.data());
  if (!pinned) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
  pinned = false;
  for (const auto &build : mat_compat::dolby_builds)
    pinned = pinned || (fileHash(enc, build.encoder_sha256.data()) && fileHash(engine, build.engine_sha256.data()));
  if (!pinned) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
  HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
  if (FAILED(hr)) return hr;
  s.ro = true;
  s.vendor = LoadLibraryExW(enc, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!s.vendor) return HRESULT_FROM_WIN32(GetLastError());
  ++s.out->vendorLoads;
  auto getFactory = reinterpret_cast<HRESULT(WINAPI *)(HSTRING, IActivationFactory **)>(GetProcAddress(s.vendor, "DllGetActivationFactory"));
  if (!getFactory) return E_NOINTERFACE;
  HSTRING name {};
  hr = WindowsCreateString(L"Dolby.DolbyMatEncoder", 21, &name);
  if (SUCCEEDED(hr)) hr = getFactory(name, &s.factory);
  if (name) WindowsDeleteString(name);
  if (hr != S_OK) return hr;
  hr = s.factory->ActivateInstance(&s.object);
  ++s.out->activations;
  if (hr != S_OK) return hr;
  hr = s.object->QueryInterface(guid(L"{deff1192-f581-4d77-9c1b-3e596b0ca989}"), reinterpret_cast<void **>(s.encoder.GetAddressOf()));
  if (hr != S_OK) return hr;
  const uintptr_t rvas[] { 0x7780, 0x7540, 0x77e0, 0x41a0, 0x5ee0, 0x6270, 0x5760, 0x4e00 };
  auto table = *reinterpret_cast<void ***>(s.encoder.Get());
  for (unsigned i = 0; i < 8; ++i)
    if (table[i] != reinterpret_cast<BYTE *>(s.vendor) + rvas[i]) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
  const BYTE prologue[] { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56 };
  if (memcmp(audio + 0xdf298, prologue, sizeof(prologue))) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
  ComPtr<IUnknown> mapperFactory;
  using Make = HRESULT(__fastcall *)(IUnknown **);
  using Create = HRESULT(__fastcall *)(IUnknown *, REFGUID, IUnknown **);
  hr = reinterpret_cast<Make>(audio + 0xdf298)(&mapperFactory);
  if (hr != S_OK) return hr;
  auto mt = *reinterpret_cast<void ***>(mapperFactory.Get());
  if (mt[3] != audio + 0xe00e0) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
  s.out->mapperHr = reinterpret_cast<Create>(mt[3])(mapperFactory.Get(), guid(L"{a289735d-fa3e-4e35-9d7d-b6f896acb2e7}"), &s.mapper);
  if (s.out->mapperHr != S_OK) return s.out->mapperHr;
  using Initialize = HRESULT(__fastcall *)(void *, UINT32, UINT32, UINT32, UINT32, ULONG, IUnknown *, REFGUID, IUnknown *);
  ++s.out->initializations;
  s.out->initializeHr = reinterpret_cast<Initialize>(table[3])(s.encoder.Get(), 480, 48000, bed714Mask, 8, 0, s.mapper.Get(), GUID_NULL, nullptr);
  s.target = s.encoder.Get();
  s.submit = reinterpret_cast<Submit>(table[4]);
  s.process = reinterpret_cast<Process>(table[7]);
  return s.out->initializeHr;
}
static HRESULT __fastcall
fakeSubmit(void *target, const void *stream, const void *properties, const float *pcm, UINT32 count, float gain) {
  auto &s = *static_cast<Session *>(target);
  if (s.config.failSubmitAt >= 0 && s.out->submissions == static_cast<uint32_t>(s.config.failSubmitAt)) return E_ABORT;
  if (!s.expected || stream != s.stream || count != 480 || gain != 1.f || s.pending >= 64) return E_FAIL;
  auto &c = *s.expected;
  Properties correct {};
  correct.id = c.id;
  correct.frames = c.frames;
  correct.type = c.type;
  correct.flags = c.flags;
  correct.x = c.xyz[0];
  correct.y = c.xyz[1];
  correct.z = c.xyz[2];
  correct.volume = 1;
  if (memcmp(properties, &correct, sizeof(correct)) || memcmp(pcm, c.pcm, sizeof(c.pcm))) return E_FAIL;
  for (uint32_t n = 0; n < s.pending; ++n)
    if (s.borrowed[n] == pcm) return E_FAIL;
  s.borrowed[s.pending] = pcm;
  s.recorded[s.pending++] = c;
  ++s.out->fakeCallsVerified;
  return S_OK;
}
static HRESULT __fastcall
fakeProcess(void *target, UINT32 bytes, BYTE *, UINT32 *produced) {
  auto &s = *static_cast<Session *>(target);
  if (bytes != request || !produced) return E_FAIL;
  if (s.config.failProcessAt >= 0 && s.out->processes == static_cast<uint32_t>(s.config.failProcessAt)) return S_FALSE;
  for (uint32_t n = 0; n < s.pending; ++n)
    if (memcmp(s.borrowed[n], s.recorded[n].pcm, sizeof(Call::pcm))) return E_FAIL;
  *produced = s.config.oversizedProcess ? request + 1 : 0;
  return S_OK;
}
static bool
submit(Session &s, const Call &c) {
  // The pinned encoder's static branch reads Properties+4 as the PCM count,
  // independently of Submit's count argument (the dynamic branch ignores it).
  if (stopped(s)) return false;
  Properties p {};
  p.id = c.id;
  p.frames = c.frames;
  p.type = c.type;
  p.flags = c.flags;
  p.x = c.xyz[0];
  p.y = c.xyz[1];
  p.z = c.xyz[2];
  p.volume = 1;
  s.expected = &c;
  s.out->submitHr = s.submit(s.target, s.stream, &p, c.pcm, 480, 1.f);
  s.expected = nullptr;
  if (s.out->submitHr != S_OK) return false;
  ++s.out->submissions;
  return true;
}
static bool
waitUntil(Session &s, HANDLE timer, int64_t target) {
  for (;;) {
    if (stopped(s)) return false;
    auto left = target - qpc();
    if (left <= 0) return true;
    LARGE_INTEGER due {};
    due.QuadPart = -((left * 10000000 + hz() - 1) / hz());
    if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) || WaitForSingleObject(timer, 20) == WAIT_FAILED) return false;
  }
}
static bool
process(Session &s, HANDLE timer, int64_t &next, bool &started, uint32_t phase, uint32_t source) {
  auto &o = *s.out;
  if (o.processes >= UINT32_MAX - 100) return false;
  if (!s.fake || s.config.paceMs) {
    auto now = qpc();
    if (!started || now - next > hz() / 500) {
      next = now;
      started = true;
    }
    if (!waitUntil(s, timer, next)) return false;
    next += hz() / 100;
  }
  UINT32 produced = 0;
  o.processHr = s.process(s.target, request, s.fake ? nullptr : s.scratch, &produced);
  auto end = qpc();
  if (o.processHr != S_OK || produced > request || (!s.fake && produced != request)) return false;
  Block b {};
  b.phase = phase;
  b.sampleStart = o.processes * 480;
  b.completedQpc = end;
  b.sourceIndex = source;
  b.frames = 480;
  const BYTE *data = s.scratch;
  b.bytes = produced;
  if (s.fake) {
    auto p = s.transcript;
    auto add = [&](const void *value, size_t n) {memcpy(p,value,n);p+=n; };
    add(&phase, 4);
    add(&source, 4);
    add(&s.pending, 4);
    for (uint32_t n = 0; n < s.pending; ++n) {
      auto &c = s.recorded[n];
      add(&c.id, 4);
      add(&c.flags, 4);
      add(c.xyz, 12);
      add(c.pcm, sizeof(c.pcm));
    }
    b.bytes = static_cast<uint32_t>(p - s.transcript);
    data = s.transcript;
  }
  // Only the dedicated codec worker may wait. Native budget is 20ms. The
  // explicitly unpaced FAKE fixture allows 1s for disk diagnostic scheduling.
  // Neither mode waits on a capture callback or overwrites a full slot.
  auto deadline = GetTickCount64() + (s.fake && !s.config.paceMs ? 1000 : 20);
  while (get(s.output->write) - get(s.output->read) >= spatial_stream::slots && GetTickCount64() < deadline) {
    if (stopped(s)) return false;
    Sleep(1);
  }
  if (!publish(*s.output, b, data)) {
    auto why = get(s.output->error);
    fail(*s.input, why ? why : codec);
    return false;
  }
  ++o.records;
  o.outputBytes += b.bytes;
  s.pending = 0;
  ++o.processes;
  if (phase == 1) ++o.warmRounds;
  if (phase == 2) ++o.sourceRounds;
  if (phase == 3) ++o.drainRounds;
  return true;
}
struct ActiveObject {
  uint32_t id, type;
};
static bool
validCalls(const Slot &slot, ActiveObject *active, uint32_t &activeCount) {
  const auto &m = slot.header;
  if (m.calls > 64 || m.frames != 480 || (m.kind != 1 && m.kind != 2)) return false;
  for (uint32_t n = 0; n < m.calls; ++n) {
    const auto &c = slot.calls[n];
    if (!c.id || !objectType(c.type) || c.frames != 480 || (c.flags != createFlags(c.type) && c.flags != updateFlags(c.type) && c.flags != 8) || (m.kind == 2 && c.flags != 8)) return false;
    for (uint32_t j = 0; j < n; ++j)
      if (slot.calls[j].id == c.id) return false;
    for (auto f : c.xyz)
      if (!std::isfinite(f) || (c.type == 1 ? fabsf(f) > 10000 : f != 0)) return false;
    for (auto f : c.pcm)
      if (!std::isfinite(f) || fabsf(f) > 1 || (c.flags == 8 && f != 0)) return false;
    uint32_t k = 0;
    while (k < activeCount && active[k].id != c.id) ++k;
    if (c.flags == createFlags(c.type)) {
      if (k < activeCount || activeCount == 20) return false;
      if (c.type != 1)
        for (uint32_t j = 0; j < activeCount; ++j)
          if (active[j].type == c.type) return false;
      active[activeCount++] = { c.id, c.type };
    }
    else {
      if (k == activeCount || active[k].type != c.type) return false;
      if (c.flags == 8) active[k] = active[--activeCount];
    }
  }
  if (m.kind == 2) return activeCount == 0;
  for (uint32_t n = 0; n < activeCount; ++n) {
    bool seen = false;
    for (uint32_t j = 0; j < m.calls; ++j) seen |= slot.calls[j].id == active[n].id;
    if (!seen) return false;
  }
  return true;
}
static DWORD WINAPI
codecWorker(void *opaque) {
  auto &s = *static_cast<Session *>(opaque);
  auto &o = *s.out;
  o.workerTid = GetCurrentThreadId();
  bool okay = false;
  HANDLE timer {};
  Slot *local {};
  Slot *prepared {};
  ActiveObject active[20] {};
  uint32_t activeCount = 0, nextSource = 0;
  bool warmed = false, started = false;
  int64_t next = 0;
  if (s.fake) {
    s.target = &s;
    s.submit = fakeSubmit;
    s.process = fakeProcess;
    o.mapperHr = S_OK;
    ++o.initializations;
    o.initializeHr = s.config.failInitialize ? S_FALSE : S_OK;
    o.createHr = S_OK;
  }
  else {
    BYTE *audio {};
    o.createHr = hostGuard(&audio) ? E_ACCESSDENIED : nativeOpen(s, audio);
  }
  if (o.createHr != S_OK || o.initializeHr != S_OK) goto done;
  timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2, TIMER_ALL_ACCESS);
  if (!timer) goto done;
  {
    local = new (std::nothrow) Slot;
    prepared = new (std::nothrow) Slot;
    if (!local || !prepared) goto done;
    for (;;) {
      if (stopped(s)) break;
      auto r = get(s.read), w = get(s.write);
      if (r == w) {
        if (WaitForSingleObject(s.available, 20) == WAIT_FAILED) break;
        continue;
      }
      memcpy(local, &s.slots[r % 8], sizeof(Slot));
      put(s.read, r + 1);
      SetEvent(s.space);
      auto &m = local->header;
      if (m.index != nextSource || (s.args.maxSourceRounds && m.index > s.args.maxSourceRounds) || !validCalls(*local, active, activeCount)) break;
      bool first = false;
      if (!warmed && m.calls && m.kind == 1) {
        for (uint32_t j = 0; j < m.calls; ++j)
          if (local->calls[j].flags != createFlags(local->calls[j].type)) goto stop;
        for (uint32_t n = 0; n < 20; ++n) {
          for (uint32_t j = 0; j < m.calls; ++j) {
            auto &c = prepared->calls[j];
            c = local->calls[j];
            memset(c.pcm, 0, sizeof(c.pcm));
            c.flags = n ? updateFlags(c.type) : createFlags(c.type);
            if (!submit(s, c)) goto stop;
          }
          if (!process(s, timer, next, started, 1, UINT32_MAX)) goto stop;
        }
        warmed = true;
        first = true;
      }
      // The native ABI may borrow PCM until Process returns. Each submitted
      // object's storage stays distinct and stable for the entire round.
      for (uint32_t j = 0; j < m.calls; ++j) {
        auto &c = prepared->calls[j];
        c = local->calls[j];
        if (first) c.flags = updateFlags(c.type);
        if (!submit(s, c)) goto stop;
        if (m.kind == 1 && c.flags != 8) o.sourceSamples += 480;
      }
      if (m.kind == 2) {
        o.eofSeen = 1;
        auto drain = drainCount(o.processes, s.fake);
        for (uint32_t n = 0; n < drain; ++n)
          if (!process(s, timer, next, started, 3, UINT32_MAX)) goto stop;
        okay = true;
        break;
      }
      if (nextSource >= UINT32_MAX - 100 || (s.args.maxSourceRounds && nextSource >= s.args.maxSourceRounds) || !process(s, timer, next, started, 2, nextSource++)) break;
    }
  stop:;
  }
done:
  if (timer) CloseHandle(timer);
  s.encoder.Reset();
  s.mapper.Reset();
  s.object.Reset();
  s.factory.Reset();
  if (s.ro) RoUninitialize();
  // Also keep PCM alive while releasing a failed native instance that may
  // still hold the final unprocessed Submit pointers.
  delete prepared;
  delete local;
  if (!okay) {
    if (!get(s.input->common.fault)) fail(*s.input, codec);
    put(s.stop, 1);
    SetEvent(s.space);
    SetEvent(s.available);
  }
  put(o.workerFinished, 1);
  return okay ? 0 : ERROR_FUNCTION_FAILED;
}
static DWORD
run(const spatial_stream::Args *input, const spatial_stream::FakeConfig *fake) {
  if (!input || !argsValid(*input)) return ERROR_INVALID_PARAMETER;
  auto *s = new (std::nothrow) Session;
  if (!s) return ERROR_OUTOFMEMORY;
  s->args = *input;
  s->fake = fake != nullptr;
  if (fake) {
    if (fake->bytes != sizeof(*fake) || fake->version != streamVersion || (fake->paceMs != 0 && fake->paceMs != 10)) {
      delete s;
      return ERROR_INVALID_PARAMETER;
    }
    s->config = *fake;
  }
  auto cleanup = [&](DWORD code) {delete s;return code; };
  HANDLE originals[] { input->input, input->data, input->cancel, input->owner, input->output };
  for (unsigned n = 0; n < 5; ++n)
    if (!DuplicateHandle(GetCurrentProcess(), originals[n], GetCurrentProcess(), &s->handles[n], 0, FALSE, DUPLICATE_SAME_ACCESS)) return cleanup(ERROR_INVALID_HANDLE);
  s->args.input = s->handles[0];
  s->args.data = s->handles[1];
  s->args.cancel = s->handles[2];
  s->args.owner = s->handles[3];
  s->args.output = s->handles[4];
  size_t bytes = sizeof(Output);
  s->output = static_cast<Output *>(MapViewOfFile(s->args.output, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
  if (!s->output) return cleanup(ERROR_INVALID_HANDLE);
  s->out = &s->output->header;
  auto &o = *s->out;
  if (!outputValid(*s->output, s->args, s->fake)) return cleanup(ERROR_INVALID_DATA);
  o.hostPid = GetCurrentProcessId();
  BYTE *audio {};
  if (!s->fake) {
    auto guard = hostGuard(&audio);
    if (guard) {
      put(o.error, guard);
      put(o.status, -1);
      return cleanup(guard);
    }
    o.hostGuardPassed = 1;
  }
  if (InterlockedCompareExchange(&running, 1, 0)) {
    put(o.error, ERROR_BUSY);
    put(o.status, -1);
    return cleanup(ERROR_BUSY);
  }
  auto finish = [&](DWORD code) {InterlockedExchange(&running,0);return cleanup(code); };
  s->input = static_cast<Shared *>(MapViewOfFile(s->args.input, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
  if (!s->input) {
    put(o.error, ERROR_INVALID_HANDLE);
    put(o.status, -1);
    return finish(ERROR_INVALID_HANDLE);
  }
  FILETIME created {}, exit {}, kernel {}, user {};
  uint64_t when = 0;
  if (GetProcessTimes(s->args.owner, &created, &exit, &kernel, &user)) when = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  if (!contract(*s->input, s->args) || GetProcessId(s->args.owner) != s->args.ownerPid || when != s->args.ownerCreationTime) {
    fail(*s->input, badContract);
    put(o.error, badContract);
    put(o.status, -1);
    return finish(ERROR_INVALID_DATA);
  }
  if (InterlockedCompareExchange(&s->input->common.consumerPid, GetCurrentProcessId(), 0) != 0) {
    fail(*s->input, identity);
    put(o.error, identity);
    put(o.status, -1);
    return finish(ERROR_INVALID_DATA);
  }
  if (!s->fake) {
    HMODULE pin {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(self), &pin)) return finish(GetLastError());
    o.helperPinned = 1;
  }
  s->available = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  s->space = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  s->began = GetTickCount64();
  if (!s->available || !s->space) {
    put(o.error, ERROR_NOT_ENOUGH_MEMORY);
    put(o.status, -1);
    return finish(ERROR_NOT_ENOUGH_MEMORY);
  }
  put(o.status, 1);
  put(s->input->reader.heartbeat, qpc());
  s->worker = CreateThread(nullptr, 0, codecWorker, s, 0, nullptr);
  if (!s->worker) {
    put(o.error, GetLastError());
    put(o.status, -1);
    return finish(GetLastError());
  }
  bool eof = false;
  uint64_t consumed = 0;
  while (!eof && !stopped(*s)) {
    put(s->input->reader.heartbeat, qpc());
    if (!contract(*s->input, s->args)) {
      fail(*s->input, badContract);
      break;
    }
    auto r = get(s->input->reader.read), w = get(s->input->writer.write);
    if (r < 0 || w < r || w - r > 32) {
      fail(*s->input, counters);
      break;
    }
    if (r == w) {
      if (get(s->input->common.producerDone) && get(s->input->writer.write) == r) {
        fail(*s->input, missingEOF);
        break;
      }
      HANDLE waits[] { s->args.data, s->args.cancel, s->args.owner, s->worker };
      auto v = WaitForMultipleObjects(4, waits, FALSE, 20);
      if (v == WAIT_FAILED) {
        fail(*s->input, badContract);
        break;
      }
      if (v == WAIT_OBJECT_0 + 2) fail(*s->input, producerExited);
      if (v == WAIT_OBJECT_0 + 3) break;
      continue;
    }
    auto &slot = s->input->slots[r % 32];
    Message m = slot.header;
    if (m.magic != frameMagic || m.bytes != sizeof(Message) + m.calls * sizeof(Call) || m.calls > 64 || m.frames != 480 || (m.kind != 1 && m.kind != 2) || m.reserved || m.enqueuedQpc <= 0) {
      fail(*s->input, badFrame);
      break;
    }
    if (m.domain != s->args.domain || m.epoch != s->args.epoch || m.sequence != consumed) {
      fail(*s->input, sequence);
      break;
    }
    if (checksum(m, slot.calls) != m.checksum) {
      fail(*s->input, checksumError);
      break;
    }
    // Copy to a local 8-slot buffer BEFORE releasing the shared slot. Only
    // this dedicated control receiver may wait for codec space.
    for (;;) {
      if (stopped(*s)) break;
      auto lw = get(s->write), lr = get(s->read);
      if (lw - lr < 8) {
        auto &destination = s->slots[lw % 8];
        destination.header = m;
        if (m.calls) memcpy(destination.calls, slot.calls, m.calls * sizeof(Call));
        put(s->write, lw + 1);
        if (static_cast<uint32_t>(lw - lr + 1) > o.localHighWater) o.localHighWater = static_cast<uint32_t>(lw - lr + 1);
        SetEvent(s->available);
        put(s->input->reader.read, r + 1);
        ++consumed;
        ++o.messages;
        eof = m.kind == 2;
        break;
      }
      put(s->input->reader.heartbeat, qpc());
      if (WaitForSingleObject(s->space, 20) == WAIT_FAILED) break;
    }
  }
  if (!eof || get(s->input->common.fault)) {
    put(s->stop, 1);
    SetEvent(s->available);
  }
  auto until = GetTickCount64() + 2000;
  while (WaitForSingleObject(s->worker, 20) != WAIT_OBJECT_0 && GetTickCount64() < until) {
    put(s->input->reader.heartbeat, qpc());
    if (stopped(*s)) {
      put(s->stop, 1);
      SetEvent(s->available);
    }
  }
  if (WaitForSingleObject(s->worker, 0) != WAIT_OBJECT_0) {
    o.workerTimedOut = 1;
    put(o.error, timeout);
    put(o.status, -1);
    HMODULE pin {};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(self), &pin);
    o.helperPinned = 1;
    return ERROR_TIMEOUT;
  }  // retain owned views/state, never free under a blocked worker
  DWORD code {};
  GetExitCodeThread(s->worker, &code);
  if (eof && code == 0 && (get(s->input->writer.write) != get(s->input->reader.read) || get(s->write) != get(s->read) || !get(s->input->common.producerDone))) fail(*s->input, sequence);
  bool okay = eof && code == 0 && !get(s->input->common.fault) && o.workerFinished;
  if (!okay) InterlockedCompareExchange(&s->output->error, get(s->input->common.fault), 0);
  put(s->output->completed, 1);
  put(o.error, okay ? 0 : get(s->input->common.fault));
  put(o.status, okay ? 2 : -1);
  put(s->input->common.consumerStatus, okay ? 4 : 5);
  return finish(okay ? 0 : ERROR_FUNCTION_FAILED);
}
extern "C" __declspec(dllexport) DWORD WINAPI
StartMatStreamReceiver(const spatial_stream::Args *args) { return run(args, nullptr); }
extern "C" __declspec(dllexport) DWORD WINAPI
TestMatStreamReceiverFake(const spatial_stream::Args *args, const spatial_stream::FakeConfig *config) {
  if (!config) return ERROR_INVALID_PARAMETER;
  return run(args, config);
}
BOOL WINAPI
DllMain(HINSTANCE module, DWORD reason, void *) {
  if (reason == DLL_PROCESS_ATTACH) {
    self = module;
    DisableThreadLibraryCalls(module);
  }
  return TRUE;
}
