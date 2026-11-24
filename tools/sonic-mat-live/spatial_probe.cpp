#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <spatialaudioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <stdexcept>
#include "../../src/spatial_audio/object_input_protocol.h"

using Microsoft::WRL::ComPtr;

static std::string
Utf8(const wchar_t *s) {
  int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  std::string result(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, -1, result.data(), n, nullptr, nullptr);
  return result;
}

static void
Check(HRESULT hr, const char *what) {
  if (FAILED(hr)) {
    printf("ERROR %s HRESULT=0x%08lX\n", what, static_cast<unsigned long>(hr));
    throw std::runtime_error(what);
  }
}

static std::vector<ComPtr<IMMDevice>>
Devices(DWORD states = DEVICE_STATE_ACTIVE) {
  ComPtr<IMMDeviceEnumerator> en;
  Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)), "Enumerator");
  ComPtr<IMMDeviceCollection> col;
  Check(en->EnumAudioEndpoints(eRender, states, &col), "EnumAudioEndpoints");
  UINT n = 0;
  Check(col->GetCount(&n), "GetCount");
  std::vector<ComPtr<IMMDevice>> out;
  for (UINT i = 0; i < n; ++i) {
    ComPtr<IMMDevice> d;
    Check(col->Item(i, &d), "Item");
    out.push_back(d);
  }
  return out;
}

static void
List(const std::vector<ComPtr<IMMDevice>> &devices) {
  ComPtr<IMMDeviceEnumerator> en;
  Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)), "Enumerator");
  std::wstring defaults[3];
  for (int role = 0; role < 3; ++role) {
    ComPtr<IMMDevice> d;
    LPWSTR id = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, static_cast<ERole>(role), &d)) &&
        SUCCEEDED(d->GetId(&id))) {
      defaults[role] = id;
      CoTaskMemFree(id);
    }
  }
  for (size_t i = 0; i < devices.size(); ++i) {
    DWORD state = 0;
    Check(devices[i]->GetState(&state), "Endpoint state");
    LPWSTR id = nullptr;
    Check(devices[i]->GetId(&id), "Endpoint ID");
    printf("endpoint=%s state=0x%X defaultConsole=%d defaultMultimedia=%d defaultCommunications=%d\n",
      Utf8(id).c_str(), state, defaults[0] == id, defaults[1] == id, defaults[2] == id);
    CoTaskMemFree(id);
    ComPtr<IPropertyStore> props;
    PROPVARIANT name {};
    devices[i]->OpenPropertyStore(STGM_READ, &props);
    if (props) props->GetValue(PKEY_Device_FriendlyName, &name);
    printf("[%zu] %s\n", i, Utf8(name.vt == VT_LPWSTR ? name.pwszVal : L"?").c_str());
    PropVariantClear(&name);
    if (state != DEVICE_STATE_ACTIVE) continue;  // Never activate an inactive endpoint.
    ComPtr<IAudioClient> audio;
    HRESULT mixHr = devices[i]->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audio);
    WAVEFORMATEX *mix = nullptr;
    if (SUCCEEDED(mixHr)) mixHr = audio->GetMixFormat(&mix);
    printf("  MixFormat=0x%08lX", static_cast<unsigned long>(mixHr));
    if (SUCCEEDED(mixHr)) {
      printf(" channels=%u rate=%u bits=%u tag=0x%04X", mix->nChannels, mix->nSamplesPerSec,
        mix->wBitsPerSample, mix->wFormatTag);
      CoTaskMemFree(mix);
    }
    printf("\n");
    ComPtr<ISpatialAudioClient> client;
    HRESULT hr = devices[i]->Activate(__uuidof(ISpatialAudioClient), CLSCTX_ALL, nullptr, &client);
    printf("  ActivateSpatialClient=0x%08lX", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
      UINT32 maxObjects = 0;
      AudioObjectType mask {};
      HRESULT dh = client->GetMaxDynamicObjectCount(&maxObjects);
      HRESULT mh = client->GetNativeStaticObjectTypeMask(&mask);
      printf(" maxDynamic=%u(0x%08lX) staticMask=0x%X(0x%08lX)\n", maxObjects,
        static_cast<unsigned long>(dh), mask, static_cast<unsigned long>(mh));
      ComPtr<IAudioFormatEnumerator> formats;
      hr = client->GetSupportedAudioObjectFormatEnumerator(&formats);
      if (SUCCEEDED(hr)) {
        UINT32 count = 0;
        formats->GetCount(&count);
        for (UINT32 f = 0; f < count; ++f) {
          WAVEFORMATEX *fmt = nullptr;
          if (SUCCEEDED(formats->GetFormat(f, &fmt))) {
            printf("  format[%u] %uch %uHz %ubit tag=0x%04X align=%u cb=%u\n",
              f, fmt->nChannels, fmt->nSamplesPerSec, fmt->wBitsPerSample, fmt->wFormatTag,
              fmt->nBlockAlign, fmt->cbSize);
            CoTaskMemFree(fmt);
          }
        }
      }
    }
    else
      printf("\n");
  }
}

static void
AccessAudit() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  PROCESSENTRY32W pe {};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) do {
      if (_wcsicmp(pe.szExeFile, L"audiodg.exe") != 0) continue;
      printf("audiodg PID=%lu\n", pe.th32ProcessID);
      for (DWORD rights : { DWORD(PROCESS_QUERY_LIMITED_INFORMATION),
             DWORD(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ),
             DWORD(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION) }) {
        SetLastError(0);
        HANDLE h = OpenProcess(rights, FALSE, pe.th32ProcessID);
        printf("  OpenProcess rights=0x%lX success=%d error=%lu\n", rights, h != nullptr, GetLastError());
        if (h) {
          PROCESS_PROTECTION_LEVEL_INFORMATION protection {};
          if (GetProcessInformation(h, ProcessProtectionLevelInfo, &protection, sizeof(protection)))
            printf("  protectionLevel=0x%08lX (NONE=0xFFFFFFFE)\n", protection.ProtectionLevel);
          PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY policy {};
          if (GetProcessMitigationPolicy(h, ProcessSignaturePolicy, &policy, sizeof(policy)))
            printf("  signaturePolicy MicrosoftOnly=%u StoreOnly=%u\n", policy.MicrosoftSignedOnly, policy.StoreSignedOnly);
          CloseHandle(h);
        }
      }
    } while (Process32NextW(snap, &pe));
  CloseHandle(snap);
}

static void
Emit(IMMDevice *dev, double seconds, unsigned objectCount, unsigned variant, bool bed714) {
  ComPtr<ISpatialAudioClient> client;
  Check(dev->Activate(__uuidof(ISpatialAudioClient), CLSCTX_ALL, nullptr, &client), "ActivateSpatialClient");
  constexpr auto bedMask = AudioObjectType_FrontLeft | AudioObjectType_FrontRight | AudioObjectType_FrontCenter | AudioObjectType_LowFrequency |
                           AudioObjectType_SideLeft | AudioObjectType_SideRight | AudioObjectType_BackLeft | AudioObjectType_BackRight |
                           AudioObjectType_TopFrontLeft | AudioObjectType_TopFrontRight | AudioObjectType_TopBackLeft | AudioObjectType_TopBackRight;
  static_assert(static_cast<unsigned>(bedMask) == receiver::bed714Mask);
  if (bed714) {
    AudioObjectType supported {};
    Check(client->GetNativeStaticObjectTypeMask(&supported), "GetStaticMask");
    if ((supported & bedMask) != bedMask) throw std::runtime_error("Full 7.1.4 static bed unavailable; no downmix or substitution");
  }
  ComPtr<IAudioFormatEnumerator> formats;
  Check(client->GetSupportedAudioObjectFormatEnumerator(&formats), "GetObjectFormats");
  WAVEFORMATEX *fmt = nullptr;
  Check(formats->GetFormat(0, &fmt), "GetFormat");
  if (fmt->nChannels != 1 || fmt->wBitsPerSample != 32 || fmt->wFormatTag != WAVE_FORMAT_IEEE_FLOAT) {
    CoTaskMemFree(fmt);
    throw std::runtime_error("Emitter requires mono float32");
  }
  UINT32 rate = fmt->nSamplesPerSec;
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  SpatialAudioObjectRenderStreamActivationParams params {};
  params.ObjectFormat = fmt;
  params.StaticObjectTypeMask = bed714 ? bedMask : AudioObjectType_None;
  params.MinDynamicObjectCount = objectCount;
  params.MaxDynamicObjectCount = objectCount;
  params.Category = AudioCategory_GameEffects;
  params.EventHandle = event;
  PROPVARIANT pv {};
  pv.vt = VT_BLOB;
  pv.blob.cbSize = sizeof(params);
  pv.blob.pBlobData = reinterpret_cast<BYTE *>(&params);
  ComPtr<ISpatialAudioObjectRenderStream> stream;
  HRESULT hr = client->ActivateSpatialAudioStream(&pv, __uuidof(ISpatialAudioObjectRenderStream), &stream);
  CoTaskMemFree(fmt);
  Check(hr, "ActivateSpatialAudioStream");
  Check(stream->Start(), "Start");
  const unsigned bedCount = bed714 ? 12u : 0u, totalObjects = bedCount + objectCount;
  std::vector<ComPtr<ISpatialAudioObject>> objects(totalObjects);
  std::vector<uint64_t> digests(totalObjects, 14695981039346656037ull);
  unsigned long long total = 0;
  unsigned blocks = 0;
  ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>((seconds + 3) * 1000);
  while (total < static_cast<unsigned long long>(seconds * rate) && GetTickCount64() < deadline) {
    if (WaitForSingleObject(event, 1000) != WAIT_OBJECT_0) throw std::runtime_error("Audio event timeout");
    UINT32 available = 0, frames = 0;
    Check(stream->BeginUpdatingAudioObjects(&available, &frames), "BeginUpdating");
    double t = static_cast<double>(total) / rate;
    for (unsigned i = 0; i < totalObjects; ++i) {
      auto &object = objects[i];
      const bool isStatic = i < bedCount;
      const unsigned dynamicIndex = isStatic ? 0 : i - bedCount;
      auto type = isStatic ? static_cast<AudioObjectType>(1u << (i + 1)) : AudioObjectType_Dynamic;
      if (!object) Check(stream->ActivateSpatialAudioObject(type, &object), "ActivateObject");
      double phase = dynamicIndex + variant * 2.0;
      float x = static_cast<float>(std::sin(t * 1.1 + phase));
      float y = static_cast<float>(0.5 * std::sin(t * 0.7 + phase));
      float z = static_cast<float>(-std::cos(t * 1.1 + phase));
      BYTE *data = nullptr;
      UINT32 bytes = 0;
      Check(object->GetBuffer(&data, &bytes), "GetBuffer");
      if (bytes != frames * sizeof(float)) throw std::runtime_error("Unexpected object buffer size");
      float *samples = reinterpret_cast<float *>(data);
      double frequency = bed714 ? (isStatic ? (type == AudioObjectType_LowFrequency ? 60.0 : 211.0 + 37.0 * i) : 907.0 + 113.0 * dynamicIndex) :
                                  (i ? 783.99 : 523.25) + variant * 100.0;
      for (UINT32 s = 0; s < frames; ++s) {
        samples[s] = static_cast<float>(0.2 * std::sin(2.0 * 3.141592653589793 * frequency * (total + s) / rate));
      }
      for (UINT32 n = 0; n < bytes; ++n) {
        digests[i] ^= data[n];
        digests[i] *= 1099511628211ull;
      }
      // The first GetBuffer can initialize/reset cached properties.
      // Apply this packet's final position/volume after that initialization.
      if (!isStatic) Check(object->SetPosition(x, y, z), "SetPosition");
      Check(object->SetVolume(isStatic ? 0.10f : (dynamicIndex ? 0.07f : 0.10f)), "SetVolume");
      if (total + frames >= static_cast<unsigned long long>(seconds * rate))
        Check(object->SetEndOfStream(frames), "SetEndOfStream");
    }
    Check(stream->EndUpdatingAudioObjects(), "EndUpdating");
    total += frames;
    ++blocks;
  }
  // Let the server consume the final submitted packet before Stop discards
  // pending engine work. This is source-side drain, not capture-side padding.
  Sleep(60);
  Check(stream->Stop(), "Stop");
  objects.clear();
  stream.Reset();
  CloseHandle(event);
  printf("EMITTER PID=%lu variant=%u objects=%u bedChannels=%u dynamicObjects=%u blocks=%u framesPerObject=%llu rate=%u duration=%.3f\n",
    GetCurrentProcessId(), variant, totalObjects, bedCount, objectCount, blocks, total, rate, static_cast<double>(total) / rate);
  for (unsigned i = 0; i < totalObjects; ++i) printf("SOURCE_DIGEST lane=%u type=0x%x frames=%llu calls=%u fnv64=%016llx\n", i, i < bedCount ? 1u << (i + 1) : 1u, total, blocks, digests[i]);
  AccessAudit();
}

int
wmain(int argc, wchar_t **argv) {
  SetConsoleOutputCP(CP_UTF8);
  int device = -1;
  double seconds = 5;
  bool emit = false;
  unsigned objectCount = 1;
  unsigned variant = 0;
  bool accessOnly = false;
  bool all = false;
  bool bed714 = false;
  std::wstring endpoint;
  std::wstring gateName;
  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"--dev" && i + 1 < argc)
      device = _wtoi(argv[++i]);
    else if (a == L"--seconds" && i + 1 < argc)
      seconds = _wtof(argv[++i]);
    else if (a == L"--emit")
      emit = true;
    else if (a == L"--access-only")
      accessOnly = true;
    else if (a == L"--all")
      all = true;
    else if (a == L"--bed-7.1.4")
      bed714 = true;
    else if (a == L"--endpoint" && i + 1 < argc)
      endpoint = argv[++i];
    else if (a == L"--objects" && i + 1 < argc)
      objectCount = _wtoi(argv[++i]);
    else if (a == L"--variant" && i + 1 < argc)
      variant = _wtoi(argv[++i]);
    else if (a == L"--gate" && i + 1 < argc)
      gateName = argv[++i];
    else if (a != L"--list")
      return 2;
  }
  if (!std::isfinite(seconds) || seconds <= 0 || seconds > 60) return 2;
  try {
    if (accessOnly) {
      AccessAudit();
      return 0;
    }
    if (!gateName.empty()) {
      HANDLE gate = OpenEventW(SYNCHRONIZE, FALSE, gateName.c_str());
      if (!gate) throw std::runtime_error("Open gate failed");
      DWORD wait = WaitForSingleObject(gate, 15000);
      CloseHandle(gate);
      if (wait != WAIT_OBJECT_0) throw std::runtime_error("Gate timeout");
    }
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
    if (all && emit) return 2;
    auto devices = Devices(all ? DEVICE_STATEMASK_ALL : DEVICE_STATE_ACTIVE);
    if (!endpoint.empty()) {
      if (device != -1) return 2;
      for (size_t i = 0; i < devices.size(); ++i) {
        LPWSTR id = nullptr;
        Check(devices[i]->GetId(&id), "Endpoint ID");
        bool match = endpoint == id;
        CoTaskMemFree(id);
        if (match) {
          device = static_cast<int>(i);
          break;
        }
      }
      if (device == -1) throw std::runtime_error("Explicit endpoint is not active; no reselection");
    }
    if (emit) {
      if (device < 0 || device >= static_cast<int>(devices.size())) return 2;
      if ((!bed714 && objectCount < 1) || objectCount > 2) return 2;
      if (variant > 1) return 2;
      Emit(devices[device].Get(), seconds, objectCount, variant, bed714);
    }
    else {
      List(devices);
      AccessAudit();
    }
    return 0;
  }
  catch (const std::exception &e) {
    printf("FAILED: %s\n", e.what());
    return 1;
  }
}
