// Independent corrected test source. Keep the original source/exe frozen as
// provenance for native01. Reuse its device listing and HRESULT helpers only.
#define wmain spatial_probe_legacy_wmain
#include "spatial_probe.cpp"
#undef wmain

template<class Object>
static void OrderedBufferAndProperties(Object* object, UINT32 frames,
                                       float x, float y, float z, float volume,
                                       BYTE** data) {
    UINT32 bytes = 0;
    Check(object->GetBuffer(data, &bytes), "GetBuffer");
    if (bytes != frames * sizeof(float)) throw std::runtime_error("Unexpected object buffer size");
    // On the matched AudioSes build, the first GetBuffer initializes XYZ=0.
    // Setters must follow it, still within the Begin/EndUpdating scope.
    Check(object->SetPosition(x, y, z), "SetPosition");
    Check(object->SetVolume(volume), "SetVolume");
}

static int OrderSelftest() {
    struct Fake {
        float x = 99, y = 99, z = 99, volume = 1;
        float samples[480]{};
        int step = 0;
        HRESULT GetBuffer(BYTE** data, UINT32* bytes) {
            if (step++ != 0) return E_UNEXPECTED;
            x = y = z = 0; *data = reinterpret_cast<BYTE*>(samples); *bytes = sizeof(samples); return S_OK;
        }
        HRESULT SetPosition(float a, float b, float c) {
            if (step++ != 1) return E_UNEXPECTED;
            x = a; y = b; z = c; return S_OK;
        }
        HRESULT SetVolume(float value) {
            if (step++ != 2) return E_UNEXPECTED;
            volume = value; return S_OK;
        }
    } fake;
    BYTE* buffer = nullptr;
    OrderedBufferAndProperties(&fake, 480, .25f, .5f, -1.f, .1f, &buffer);
    if (fake.step != 3 || fake.x != .25f || fake.y != .5f || fake.z != -1.f ||
        fake.volume != .1f || buffer != reinterpret_cast<BYTE*>(fake.samples)) return 1;
    puts("PASS GetBuffer -> SetPosition -> SetVolume; fake startup clear cannot erase coordinates; no COM/audio/process access");
    return 0;
}

static void EmitOrdered(IMMDevice* device, unsigned seconds, unsigned count, unsigned variant) {
    ComPtr<ISpatialAudioClient> client;
    Check(device->Activate(__uuidof(ISpatialAudioClient), CLSCTX_ALL, nullptr, &client), "ActivateSpatialClient");
    ComPtr<IAudioFormatEnumerator> formats;
    Check(client->GetSupportedAudioObjectFormatEnumerator(&formats), "GetObjectFormats");
    WAVEFORMATEX* format = nullptr;
    Check(formats->GetFormat(0, &format), "GetFormat");
    bool correct = format->nChannels == 1 && format->wBitsPerSample == 32 &&
                   format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format->nSamplesPerSec == 48000;
    SpatialAudioObjectRenderStreamActivationParams params{};
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!correct || !event) { CoTaskMemFree(format); if (event) CloseHandle(event); throw std::runtime_error("Requires 48k mono F32 and event"); }
    params.ObjectFormat = format; params.MinDynamicObjectCount = count;
    params.MaxDynamicObjectCount = count; params.Category = AudioCategory_GameEffects;
    params.EventHandle = event;
    PROPVARIANT pv{}; pv.vt = VT_BLOB; pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    ComPtr<ISpatialAudioObjectRenderStream> stream;
    HRESULT activated = client->ActivateSpatialAudioStream(&pv, __uuidof(ISpatialAudioObjectRenderStream), &stream);
    CoTaskMemFree(format);
    if (FAILED(activated)) { CloseHandle(event); Check(activated, "ActivateSpatialAudioStream"); }
    try {
        Check(stream->Start(), "Start");
        std::vector<ComPtr<ISpatialAudioObject>> objects(count);
        unsigned long long total = 0;
        unsigned blocks = 0;
        ULONGLONG deadline = GetTickCount64() + (seconds + 3) * 1000;
        while (total < seconds * 48000ull && GetTickCount64() < deadline) {
            if (WaitForSingleObject(event, 1000) != WAIT_OBJECT_0) throw std::runtime_error("Audio event timeout");
            UINT32 available = 0, frames = 0;
            Check(stream->BeginUpdatingAudioObjects(&available, &frames), "BeginUpdating");
            if (frames != 480) throw std::runtime_error("Ordered oracle requires 480-frame source ticks");
            double t = static_cast<double>(total) / 48000;
            for (unsigned i = 0; i < count; ++i) {
                auto& object = objects[i];
                if (!object) Check(stream->ActivateSpatialAudioObject(AudioObjectType_Dynamic, &object), "ActivateObject");
                double phase = i + variant * 2.0;
                float x = static_cast<float>(std::sin(t * 1.1 + phase));
                float y = static_cast<float>(.5 * std::sin(t * .7 + phase));
                float z = static_cast<float>(-std::cos(t * 1.1 + phase));
                BYTE* data = nullptr;
                OrderedBufferAndProperties(object.Get(), frames, x, y, z, i ? .07f : .10f, &data);
                double frequency = (i ? 783.99 : 523.25) + variant * 100.0;
                auto* samples = reinterpret_cast<float*>(data);
                for (UINT32 s = 0; s < frames; ++s)
                    samples[s] = static_cast<float>(.2 * std::sin(2.0 * 3.141592653589793 * frequency * (total + s) / 48000));
                if (total + frames >= seconds * 48000ull) Check(object->SetEndOfStream(frames), "SetEndOfStream");
            }
            Check(stream->EndUpdatingAudioObjects(), "EndUpdating");
            total += frames; ++blocks;
        }
        if (total != seconds * 48000ull) throw std::runtime_error("Incomplete source deadline");
        Sleep(60);
        Check(stream->Stop(), "Stop");
        objects.clear(); stream.Reset();
        printf("EMITTER PID=%lu variant=%u objects=%u blocks=%u framesPerObject=%llu rate=48000 duration=%.3f\n",
               GetCurrentProcessId(), variant, count, blocks, total, static_cast<double>(total) / 48000);
        puts("SOURCE_ORDER GetBuffer -> SetPosition -> SetVolume; source PCM formula unchanged; Windows fade not bypassed");
    } catch (...) { stream.Reset(); CloseHandle(event); throw; }
    CloseHandle(event);
}

int wmain(int argc, wchar_t** argv) {
    // Diagnostic modes return before COM/device activation. No default playback.
    if (argc == 2 && std::wstring(argv[1]) == L"--selftest") return OrderSelftest();
    if (argc == 2 && std::wstring(argv[1]) == L"--describe") {
        puts("Independent source; explicit --emit --dev 0..3 --objects 1|2 --seconds 1|2|3 --variant 0|1 required. 48k F32/480 frames. GetBuffer before setters. No native retest performed by --describe."); return 0;
    }
    if (argc != 10 || std::wstring(argv[1]) != L"--emit") return 2;
    int device = -1, objects = -1, seconds = -1, variant = -1;
    for (int index = 2; index < argc; index += 2) {
        std::wstring key = argv[index], value = argv[index + 1];
        if (value.size() != 1 || value[0] < L'0' || value[0] > L'3') return 2;
        int number = value[0] - L'0';
        int* target = key == L"--dev" ? &device : key == L"--objects" ? &objects :
                      key == L"--seconds" ? &seconds : key == L"--variant" ? &variant : nullptr;
        if (!target || *target != -1) return 2;
        *target = number;
    }
    if (device < 0 || objects < 1 || objects > 2 || seconds < 1 || seconds > 3 || variant < 0 || variant > 1) return 2;
    bool initialized = false; int result = 1;
    try {
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx"); initialized = true;
        auto devices = Devices();
        if (device >= static_cast<int>(devices.size())) throw std::runtime_error("Endpoint missing");
        EmitOrdered(devices[device].Get(), seconds, objects, variant); result = 0;
    } catch (const std::exception& e) { printf("FAILED: %s\n", e.what()); }
    if (initialized) CoUninitialize();
    return result;
}
