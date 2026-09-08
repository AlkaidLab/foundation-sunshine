/**
 * @file src/platform/windows/mic_write.cpp
 * @brief Implementation for Windows microphone write functionality.
 */
#define INITGUID

// Platform includes
#include <audioclient.h>
#include <avrt.h>
#include <cmath>
#include <filesystem>
#include <mmdeviceapi.h>
#include <mutex>
#include <numbers>
#include <roapi.h>
#include <synchapi.h>
#include <urlmon.h>
#include <winreg.h>

// Local includes
#include "mic_write.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/windows/virtual_device_host/microphone_client.h"

// Must be the last included file
// clang-format off
#include "PolicyConfig.h"
// clang-format on

// Property key definitions
DEFINE_PROPERTYKEY(PKEY_Device_DeviceDesc, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 2);
DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);
DEFINE_PROPERTYKEY(PKEY_DeviceInterface_FriendlyName, 0x026e516e, 0xb814, 0x414b, 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22, 2);

namespace platf::audio {

  namespace {
    bool
    is_mic_device_lost(HRESULT status) noexcept {
      return status == AUDCLNT_E_DEVICE_INVALIDATED ||
             status == AUDCLNT_E_RESOURCES_INVALIDATED ||
             status == AUDCLNT_E_SERVICE_NOT_RUNNING;
    }
  }  // namespace

  template <class T>
  void
  co_task_free(T *p) {
    if (p) {
      CoTaskMemFree(p);
    }
  }

  using device_enum_t = util::safe_ptr<IMMDeviceEnumerator, Release<IMMDeviceEnumerator>>;
  using device_t = util::safe_ptr<IMMDevice, Release<IMMDevice>>;
  using collection_t = util::safe_ptr<IMMDeviceCollection, Release<IMMDeviceCollection>>;
  using audio_client_t = util::safe_ptr<IAudioClient, Release<IAudioClient>>;
  using wstring_t = util::safe_ptr<WCHAR, co_task_free<WCHAR>>;
  using policy_t = util::safe_ptr<IPolicyConfig, Release<IPolicyConfig>>;
  using prop_t = util::safe_ptr<IPropertyStore, Release<IPropertyStore>>;

  class prop_var_t {
  public:
    prop_var_t() {
      PropVariantInit(&prop);
    }

    ~prop_var_t() {
      PropVariantClear(&prop);
    }

    PROPVARIANT prop;
  };

  // mic_write_wasapi_t implementation
  mic_write_wasapi_t::~mic_write_wasapi_t() {
    cleanup();
  }

  void
  mic_write_wasapi_t::cleanup() {
    is_cleaning_up.store(true);

    // 停止音频客户端，不在清理路径等待尾部数据。
    if (audio_client) {
      // 停止后 endpoint 不再消费已排队帧，因此不能在这里轮询 padding 等待清空。
      audio_client->Stop();
    }

    // COM 接口释放顺序很重要：
    // 1. audio_render (从 audio_client 获取的子接口)
    // 2. audio_client
    // 3. device_enum
    if (audio_render) {
      audio_render->Release();
      audio_render = nullptr;
    }

    // 显式释放 audio_client 和 device_enum，确保正确的释放顺序
    audio_client.reset();
    device_enum.reset();
    buffer_frame_count = 0;
    pcm_output_buffer.clear();

    if (mmcss_task_handle) {
      AvRevertMmThreadCharacteristics(mmcss_task_handle);
      mmcss_task_handle = nullptr;
    }

    // 注意: 不在析构函数的 cleanup 中使用 BOOST_LOG，避免静态对象析构顺序问题
  }

  capture_e
  mic_write_wasapi_t::sample(std::vector<float> &sample_out) {
    BOOST_LOG(error) << "mic_write_wasapi_t::sample() should not be called";
    return capture_e::error;
  }

  int
  mic_write_wasapi_t::init(bool test_mode) {
    // 初始化设备枚举器
    HRESULT hr = CoCreateInstance(
      CLSID_MMDeviceEnumerator,
      nullptr,
      CLSCTX_ALL,
      IID_IMMDeviceEnumerator,
      (void **) &device_enum);

    if (FAILED(hr)) {
      BOOST_LOG(error) << "Couldn't create Device Enumerator for mic write: [0x" << util::hex(hr).to_string_view() << "]";
      cleanup();
      return -1;
    }

    if (!test_mode) {
      // 存储原始音频设备设置
      store_original_audio_settings();

      // 尝试创建或使用虚拟音频设备
      if (create_virtual_audio_device() != 0) {
        BOOST_LOG(warning) << "Virtual audio device not available, microphone redirection may not work";
      }

      // 设置loopback
      if (setup_virtual_mic_loopback() != 0) {
        BOOST_LOG(warning) << "Failed to setup virtual microphone loopback";
      }
    }

    // 对于麦克风重定向，我们需要使用虚拟音频输出设备
    device_t device;

    auto vb_matched = find_device_id({ { match_field_e::adapter_friendly_name, L"VB-Audio Virtual Cable" } });
    if (vb_matched) {
      hr = device_enum->GetDevice(vb_matched->second.c_str(), &device);
      if (SUCCEEDED(hr) && device) {
        BOOST_LOG(info) << "Using VB-Audio Virtual Cable for client mic redirection";
      }
    }

    if (test_mode) {
      const auto vb_capture = find_capture_device_id({ { match_field_e::adapter_friendly_name, L"VB-Audio Virtual Cable" } });
      if (!vb_matched || !vb_capture || FAILED(hr) || !device) {
        BOOST_LOG(warning) << "Microphone route test requires active VB-Cable render and capture endpoints";
        cleanup();
        return -1;
      }
    }

    // 最后尝试使用默认的扬声器设备
    if (!test_mode && (FAILED(hr) || !device)) {
      hr = device_enum->GetDefaultAudioEndpoint(eRender, eConsole, &device);
      if (SUCCEEDED(hr) && device) {
        BOOST_LOG(info) << "Using default console audio output device for client mic redirection";
      }
    }

    if (FAILED(hr) || !device) {
      BOOST_LOG(error) << "No suitable audio output device available for client mic redirection";
      cleanup();
      return -1;
    }

    // 激活 IAudioClient
    auto status = device->Activate(
      IID_IAudioClient,
      CLSCTX_ALL,
      nullptr,
      (void **) &audio_client);
    if (FAILED(status) || !audio_client) {
      BOOST_LOG(error) << "Failed to activate IAudioClient for mic write: [0x" << util::hex(status).to_string_view() << "]";

      // 获取设备信息以便调试
      wstring_t device_id;
      device->GetId(&device_id);
      BOOST_LOG(error) << "Device ID: " << platf::to_utf8(device_id.get());

      cleanup();
      return -1;
    }

    // 尝试多种音频格式，从最兼容的开始
    std::vector<WAVEFORMATEX> formats_to_try = {
      // 16位单声道，48kHz
      { WAVE_FORMAT_PCM, 1, 48000, 96000, 2, 16, 0 },
      // 16位立体声，48kHz
      { WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 },
    };

    HRESULT init_status = E_FAIL;
    WAVEFORMATEX *used_format = nullptr;

    for (const auto &format : formats_to_try) {
      BOOST_LOG(debug) << "Trying audio format: " << format.nChannels << " channels, "
                       << format.nSamplesPerSec << " Hz, " << format.wBitsPerSample << " bits";

      init_status = audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        1000000,  // 100ms buffer (10000000 was 10 seconds)
        0,
        &format,
        nullptr);

      if (SUCCEEDED(init_status)) {
        used_format = const_cast<WAVEFORMATEX *>(&format);
        BOOST_LOG(info) << "Successfully initialized with format: " << format.nChannels << " channels, "
                        << format.nSamplesPerSec << " Hz, " << format.wBitsPerSample << " bits";
        break;
      }
      else {
        BOOST_LOG(debug) << "Format failed: [0x" << util::hex(init_status).to_string_view() << "]";
      }
    }

    if (FAILED(init_status)) {
      BOOST_LOG(error) << "Failed to initialize IAudioClient with any supported format: [0x" << util::hex(init_status).to_string_view() << "]";
      cleanup();
      return -1;
    }

    // 保存使用的格式信息
    current_format = *used_format;

    status = audio_client->GetBufferSize(&buffer_frame_count);
    if (FAILED(status) || buffer_frame_count == 0) {
      BOOST_LOG(error) << "Failed to get buffer size for mic write: [0x" << util::hex(status).to_string_view() << "]";
      cleanup();
      return -1;
    }

    // 启动音频客户端
    status = audio_client->Start();
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to start IAudioClient for mic write: [0x" << util::hex(status).to_string_view() << "]";
      cleanup();
      return -1;
    }

    // 获取 IAudioRenderClient - 用于写入音频数据
    status = audio_client->GetService(IID_IAudioRenderClient, (void **) &audio_render);
    if (FAILED(status) || !audio_render) {
      BOOST_LOG(error) << "Failed to get IAudioRenderClient for mic write: [0x" << util::hex(status).to_string_view() << "]";
      audio_client->Stop();
      cleanup();
      return -1;
    }

    // 设置MMCSS优先级
    {
      DWORD task_index = 0;
      mmcss_task_handle = AvSetMmThreadCharacteristics("Pro Audio", &task_index);
      if (!mmcss_task_handle) {
        BOOST_LOG(warning) << "Couldn't associate mic write thread with Pro Audio MMCSS task [0x" << util::hex(GetLastError()).to_string_view() << ']';
      }
    }

    BOOST_LOG(info) << "Successfully initialized mic write device";
    return 0;
  }

  int
  mic_write_wasapi_t::write_pcm(const std::int16_t *samples, std::size_t frame_count) {
    if (!audio_client || !audio_render || !samples) {
      BOOST_LOG(error) << "Mic write device not initialized";
      return -1;
    }

    const auto framesToWrite = static_cast<UINT32>(frame_count);

    // 共享模式下可用空间等于初始化时的固定 buffer size 减当前 padding。
    UINT32 padding = 0;
    auto status = audio_client->GetCurrentPadding(&padding);
    if (FAILED(status)) {
      if (is_mic_device_lost(status)) {
        BOOST_LOG(warning) << "Audio device invalidated during mic write (GetCurrentPadding)";
        return -2;  // Special return value indicating device invalidated
      }
      BOOST_LOG(error) << "Failed to get current padding for mic write: [0x" << util::hex(status).to_string_view() << "]";
      return -1;
    }

    if (padding > buffer_frame_count) {
      BOOST_LOG(warning) << "Invalid mic write padding value: " << padding << " > " << buffer_frame_count;
      return 0;
    }

    const auto availableFrames = buffer_frame_count - padding;
    if (framesToWrite == 0 || framesToWrite > availableFrames) {
      // 麦克风 UDP、混音和设备写入共用一个线程。空间不足时整帧丢弃，
      // 不能在这里等待，否则会同时阻塞所有客户端的收包。
      return 0;
    }

    // 确认端点有空间后再转换声道，避免背压丢帧路径做无效拷贝。
    if (current_format.nChannels == 1) {
      pcm_output_buffer.assign(samples, samples + frame_count);
    }
    else if (current_format.nChannels == 2) {
      pcm_output_buffer.resize(frame_count * 2);
      for (std::size_t i = 0; i < frame_count; ++i) {
        pcm_output_buffer[i * 2] = samples[i];
        pcm_output_buffer[i * 2 + 1] = samples[i];
      }
    }
    else {
      BOOST_LOG(error) << "Unsupported channel count for mic write: " << current_format.nChannels;
      return -1;
    }

    // 获取渲染缓冲区
    BYTE *pData = nullptr;
    status = audio_render->GetBuffer(framesToWrite, &pData);
    if (FAILED(status)) {
      if (status == AUDCLNT_E_BUFFER_TOO_LARGE) {
        return 0;
      }
      if (is_mic_device_lost(status)) {
        BOOST_LOG(warning) << "Audio device invalidated during mic write (GetBuffer)";
        return -2;  // Special return value indicating device invalidated
      }
      BOOST_LOG(error) << "Failed to get render buffer for mic write: [0x" << util::hex(status).to_string_view() << "]";
      return -1;
    }

    // 拷贝解码后的PCM数据到缓冲区
    memcpy(pData, pcm_output_buffer.data(), framesToWrite * current_format.nBlockAlign);

    // 释放缓冲区
    status = audio_render->ReleaseBuffer(framesToWrite, 0);
    if (FAILED(status)) {
      if (is_mic_device_lost(status)) {
        BOOST_LOG(warning) << "Audio device invalidated during mic write (ReleaseBuffer)";
        return -2;  // Special return value indicating device invalidated
      }
      BOOST_LOG(error) << "Failed to release render buffer for mic write: [0x" << util::hex(status).to_string_view() << "]";
      return -1;
    }

    return framesToWrite * current_format.nBlockAlign;  // 返回实际写入的字节数
  }

  int
  mic_write_wasapi_t::test_write() {
    if (!audio_client || !audio_render) {
      BOOST_LOG(error) << "Mic write device not initialized for test";
      return -1;
    }

    constexpr double tone_hz = 440.0;
    constexpr double amplitude = 0.18;
    constexpr double pi = 3.14159265358979323846;
    constexpr int frame_samples = 960;  // 20 ms at 48 kHz
    constexpr int packet_count = 40;  // 800 ms
    std::vector<int16_t> pcm(frame_samples);
    int total_bytes_written = 0;

    BOOST_LOG(info) << "Testing client mic redirection with an 800 ms tone";

    for (int packet_index = 0; packet_index < packet_count; ++packet_index) {
      for (int sample_index = 0; sample_index < frame_samples; ++sample_index) {
        const auto absolute_sample = packet_index * frame_samples + sample_index;
        const double phase = 2.0 * pi * tone_hz *
                             static_cast<double>(absolute_sample) / 48000.0;
        pcm[sample_index] = static_cast<int16_t>(std::sin(phase) * amplitude * 32767.0);
      }

      const int bytes_written = write_pcm(pcm.data(), pcm.size());
      if (bytes_written < 0) {
        return -1;
      }
      total_bytes_written += bytes_written;
      // 测试音保持较短的写入间隔；缓冲区已满时 write_pcm() 会丢弃当前帧，
      // 不会把测试路径的等待逻辑带回实时写入函数。
      Sleep(10);
    }

    // The writer finishes ahead of playback, so the last packets are still
    // queued. cleanup() stops the audio client outright, which would clip the
    // tail off the tone; wait for the endpoint to drain first.
    for (int drain_attempt = 0; drain_attempt < 40; ++drain_attempt) {
      UINT32 padding = 0;
      if (FAILED(audio_client->GetCurrentPadding(&padding)) || padding == 0) {
        break;
      }
      const DWORD remaining_ms = static_cast<DWORD>(
        padding * 1000ull / current_format.nSamplesPerSec);
      Sleep(std::max<DWORD>(remaining_ms, 5));
    }

    return total_bytes_written;
  }

  mic_redirect_test_result_t
  test_mic_redirect() {
    static std::mutex test_mutex;
    const std::lock_guard lock(test_mutex);

    const auto backend = config::audio.microphone_redirect_backend;
    if (!try_begin_mic_redirect_test()) {
      return { false, "MIC_TEST_BUSY", mic_redirect_status().active_backend };
    }
    struct test_reservation_t {
      ~test_reservation_t() {
        end_mic_redirect_test();
      }
    } test_reservation;
    if (backend == "disabled") {
      report_mic_redirect_backend({}, "MIC_BACKEND_DISABLED");
      return { false, "MIC_BACKEND_DISABLED", "disabled" };
    }

    std::string usbip_fallback_reason;
    if (backend == "usbip_experimental" || backend == "auto") {
      auto &client = virtual_device_host::persistent_microphone_client();
      if (client.start()) {
        report_mic_redirect_backend("usbip_experimental");
        constexpr auto sample_rate = 48'000u;
        constexpr auto frames_per_packet = 480u;
        constexpr auto packet_count = 100u;
        std::array<std::int16_t, frames_per_packet> pcm {};
        for (std::uint32_t packet = 0; packet < packet_count; ++packet) {
          for (std::uint32_t frame = 0; frame < frames_per_packet; ++frame) {
            const auto sample = packet * frames_per_packet + frame;
            pcm[frame] = static_cast<std::int16_t>(
              std::sin(sample * 2.0 * std::numbers::pi * 440.0 / sample_rate) * 4096.0);
          }
          if (client.write_pcm(pcm.data(), pcm.size()) < 0) {
            report_mic_redirect_backend({}, "MIC_USBIP_WRITE_FAILED");
            return { false, "MIC_USBIP_WRITE_FAILED", "usbip_experimental" };
          }
          Sleep(10);
        }
        if (!client.flush()) {
          report_mic_redirect_backend({}, "MIC_USBIP_FLUSH_FAILED");
          return { false, "MIC_USBIP_FLUSH_FAILED", "usbip_experimental" };
        }
        return { true, {}, "usbip_experimental" };
      }
      const auto status = virtual_device_host::microphone_status();
      usbip_fallback_reason = status.error_code.empty() ?
                                "MIC_USBIP_DEVICE_UNAVAILABLE" :
                                status.error_code;
      if (backend == "usbip_experimental") {
        report_mic_redirect_backend({}, usbip_fallback_reason);
        return { false,
                 usbip_fallback_reason,
                 "usbip_experimental" };
      }
    }

    const auto com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_status) && com_status != RPC_E_CHANGED_MODE) {
      report_mic_redirect_backend({}, "MIC_TEST_COM_FAILED");
      return { false, "MIC_TEST_COM_FAILED", "vb_cable" };
    }

    mic_redirect_test_result_t result;
    {
      mic_write_wasapi_t test_device;
      if (test_device.init(true) != 0) {
        result.error_code = "MIC_TEST_DEVICE_UNAVAILABLE";
      }
      else if (test_device.test_write() <= 0) {
        result.error_code = "MIC_TEST_WRITE_FAILED";
      }
      else {
        result.success = true;
      }
      result.backend = "vb_cable";
      report_mic_redirect_backend(result.success ? "vb_cable" : "",
                                  result.success ? usbip_fallback_reason : result.error_code);
    }

    if (SUCCEEDED(com_status)) {
      CoUninitialize();
    }
    return result;
  }

  int
  mic_write_wasapi_t::create_virtual_audio_device() {
    BOOST_LOG(info) << "Attempting to create/use virtual audio device for client mic redirection";

    // 检查VB-Cable虚拟设备
    auto vb_matched = find_device_id({ { match_field_e::adapter_friendly_name, L"VB-Audio Virtual Cable" } });
    if (vb_matched) {
      BOOST_LOG(info) << "Found existing VB-Audio Virtual Cable device";
      virtual_device_type = VirtualDeviceType::VB_CABLE;
      return 0;  // 设备已存在
    }

    BOOST_LOG(debug) << "VB-Cable not found, attempting to download...";

    // 检查是否已安装VB-Cable驱动程序
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\VB\\VBAudioVAC", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
      RegCloseKey(hKey);
      BOOST_LOG(info) << "VB-Cable driver is already installed";
      return -1;  // 已安装但未找到设备，可能是未启用
    }

    // 检查是否已经下载并解压过（防止重复下载）
    std::wstring extract_path = std::filesystem::temp_directory_path().wstring() + L"\\VBCABLE_Install";
    std::wstring installer_path = extract_path + L"\\VBCABLE_Setup_x64.exe";
    if (std::filesystem::exists(installer_path)) {
      // 安装程序已存在，只需提示用户安装
      BOOST_LOG(warning) << "VB-Cable already downloaded to: " << platf::to_utf8(extract_path) << " ; Please run 'VBCABLE_Setup_x64.exe' as administrator to install, then restart Sunshine";
      return -1;
    }

    // 下载VB-Cable
    BOOST_LOG(debug) << "Downloading VB-Cable...";

    // 下载VB-Cable安装程序
    std::wstring download_url = L"https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip";
    std::wstring temp_path = std::filesystem::temp_directory_path().wstring() + L"\\VBCABLE_Driver_Pack43.zip";

    HMODULE urlmon = LoadLibraryW(L"urlmon.dll");
    if (!urlmon) {
      BOOST_LOG(warning) << "VB-Cable is required for microphone streaming. Please download from: https://vb-audio.com/Cable/";
      return -1;
    }

    auto URLDownloadToFileW_ptr = (decltype(URLDownloadToFileW) *) GetProcAddress(urlmon, "URLDownloadToFileW");
    if (!URLDownloadToFileW_ptr || URLDownloadToFileW_ptr(nullptr, download_url.c_str(), temp_path.c_str(), 0, nullptr) != S_OK) {
      BOOST_LOG(warning) << "Failed to download VB-Cable. Please download manually from: https://vb-audio.com/Cable/";
      FreeLibrary(urlmon);
      return -1;
    }
    FreeLibrary(urlmon);

    // 解压安装包到用户可访问的位置
    std::error_code ec;
    std::filesystem::create_directories(extract_path, ec);
    if (ec && ec != std::errc::file_exists) {
      BOOST_LOG(error) << "Failed to create extraction directory: " << ec.message();
      return -1;
    }

    // 解压VB-Cable
    BOOST_LOG(debug) << "Extracting VB-Cable...";
    std::wstring extract_cmd = L"powershell -command \"Expand-Archive -Path '" + temp_path + L"' -DestinationPath '" + extract_path + L"' -Force\"";

    if (_wsystem(extract_cmd.c_str()) != 0) {
      BOOST_LOG(error) << "Failed to extract VB-Cable installer";
      return -1;
    }

    // 引导用户手动安装
    BOOST_LOG(warning) << "VB-Cable downloaded to: " << platf::to_utf8(extract_path) << " ; Please run 'VBCABLE_Setup_x64.exe' as administrator to install, then restart Sunshine";

    return -1;
  }

  std::optional<matched_field_t>
  mic_write_wasapi_t::find_device_id(const match_fields_list_t &match_list) {
    if (match_list.empty() || !device_enum) {
      return std::nullopt;
    }

    collection_t collection;
    auto status = device_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't enumerate render devices: [0x"sv << util::hex(status).to_string_view() << ']';
      return std::nullopt;
    }

    return find_device_in_collection(collection.get(), match_list);
  }

  std::optional<matched_field_t>
  mic_write_wasapi_t::find_capture_device_id(const match_fields_list_t &match_list) {
    if (match_list.empty() || !device_enum) {
      return std::nullopt;
    }

    collection_t collection;
    auto status = device_enum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't enumerate capture devices: [0x"sv << util::hex(status).to_string_view() << ']';
      return std::nullopt;
    }

    return find_device_in_collection(collection.get(), match_list);
  }

  std::optional<matched_field_t>
  mic_write_wasapi_t::find_device_in_collection(void *collection_ptr, const match_fields_list_t &match_list) {
    auto collection = static_cast<IMMDeviceCollection *>(collection_ptr);
    UINT count = 0;
    collection->GetCount(&count);

    std::vector<std::wstring> matched(match_list.size());
    for (auto x = 0; x < count; ++x) {
      device_t device;
      collection->Item(x, &device);

      wstring_t wstring_id;
      device->GetId(&wstring_id);
      std::wstring device_id = wstring_id.get();

      prop_t prop;
      device->OpenPropertyStore(STGM_READ, &prop);

      prop_var_t adapter_friendly_name;
      prop_var_t device_friendly_name;
      prop_var_t device_desc;

      prop->GetValue(PKEY_Device_FriendlyName, &device_friendly_name.prop);
      prop->GetValue(PKEY_DeviceInterface_FriendlyName, &adapter_friendly_name.prop);
      prop->GetValue(PKEY_Device_DeviceDesc, &device_desc.prop);

      for (size_t i = 0; i < match_list.size(); i++) {
        if (matched[i].empty()) {
          const wchar_t *match_value = nullptr;
          switch (match_list[i].first) {
            case match_field_e::device_id:
              match_value = device_id.c_str();
              break;

            case match_field_e::device_friendly_name:
              match_value = device_friendly_name.prop.pwszVal;
              break;

            case match_field_e::adapter_friendly_name:
              match_value = adapter_friendly_name.prop.pwszVal;
              break;

            case match_field_e::device_description:
              match_value = device_desc.prop.pwszVal;
              break;
          }
          if (match_value && std::wcscmp(match_value, match_list[i].second.c_str()) == 0) {
            matched[i] = device_id;
          }
        }
      }
    }

    for (size_t i = 0; i < match_list.size(); i++) {
      if (!matched[i].empty()) {
        return matched_field_t(match_list[i].first, matched[i]);
      }
    }

    return std::nullopt;
  }

  HRESULT
  mic_write_wasapi_t::set_default_device_all_roles(const std::wstring &device_id) {
    IPolicyConfig *policy_raw = nullptr;
    HRESULT hr = CoCreateInstance(
      CLSID_CPolicyConfigClient,
      nullptr,
      CLSCTX_ALL,
      IID_IPolicyConfig,
      (void **) &policy_raw);

    if (FAILED(hr) || !policy_raw) {
      BOOST_LOG(error) << "Couldn't create PolicyConfig instance: [0x" << util::hex(hr).to_string_view() << "]";
      return hr;
    }

    policy_t policy(policy_raw);
    hr = policy->SetDefaultEndpoint(device_id.c_str(), eCommunications);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to set device as default communications device: [0x" << util::hex(hr).to_string_view() << "]";
      return hr;
    }

    hr = policy->SetDefaultEndpoint(device_id.c_str(), eConsole);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to set device as default console device: [0x" << util::hex(hr).to_string_view() << "]";
      return hr;
    }

    return S_OK;
  }

  int
  mic_write_wasapi_t::setup_virtual_mic_loopback() {
    if (virtual_device_type == VirtualDeviceType::NONE) {
      BOOST_LOG(warning) << "No virtual device available for loopback setup";
      return -1;
    }

    BOOST_LOG(info) << "Setting up virtual microphone loopback for client mic redirection";

    // 根据虚拟设备类型设置循环
    switch (virtual_device_type) {
      case VirtualDeviceType::STEAM:
        return setup_steam_mic_loopback();
      case VirtualDeviceType::VB_CABLE:
        return setup_vb_cable_mic_loopback();
      default:
        BOOST_LOG(warning) << "Unknown virtual device type for loopback setup";
        return -1;
    }
  }

  int
  mic_write_wasapi_t::setup_steam_mic_loopback() {
    BOOST_LOG(info) << "Setting up Steam virtual microphone loopback";

    // Steam Streaming Speakers 会自动循环到 Steam Streaming Microphone
    // 我们需要确保Steam Streaming Microphone被设置为默认录音设备
    if (auto steam_mic = find_capture_device_id({ { match_field_e::adapter_friendly_name, L"Steam Streaming Microphone" } })) {
      HRESULT hr = set_default_device_all_roles(steam_mic->second);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "Failed to set Steam Streaming Microphone as default device: [0x" << util::hex(hr).to_string_view() << "]";
        return -1;
      }
    }
    return 0;
  }

  int
  mic_write_wasapi_t::setup_vb_cable_mic_loopback() {
    BOOST_LOG(info) << "Setting up VB-Cable virtual microphone loopback";

    // 1. 检查VB-Cable输入设备是否存在
    auto vb_input = find_capture_device_id({ { match_field_e::adapter_friendly_name, L"VB-Audio Virtual Cable" } });
    if (!vb_input) {
      BOOST_LOG(warning) << "VB-Cable Input device not found";
      return -1;
    }

    // 2. 设置VB-Cable为默认录音设备
    HRESULT hr = set_default_device_all_roles(vb_input->second);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to set VB-Cable as default device: [0x" << util::hex(hr).to_string_view() << "]";
      return -1;
    }
    restoration_state.input_device_changed = true;
    BOOST_LOG(info) << "Successfully set VB-Cable as default recording device";

    // 3. 检查VB-Cable输出设备
    auto vb_output = find_device_id({ { match_field_e::adapter_friendly_name, L"VB-Audio Virtual Cable" } });
    if (!vb_output) {
      BOOST_LOG(info) << "VB-Cable output device not found, skipping output device check";
      return 0;
    }

    // 4. 检查VB-Cable是否是默认播放设备
    device_t default_device;
    if (FAILED(device_enum->GetDefaultAudioEndpoint(eRender, eConsole, &default_device)) || !default_device) {
      BOOST_LOG(warning) << "Failed to get default playback device";
      return 0;
    }

    wstring_t default_id;
    if (FAILED(default_device->GetId(&default_id))) {
      BOOST_LOG(warning) << "Failed to get default playback device ID";
      return 0;
    }

    if (default_id.get() != vb_output->second) {
      BOOST_LOG(info) << "VB-Cable is not the default playback device, no need to switch";
      return 0;
    }

    // 5. 寻找替代播放设备
    BOOST_LOG(info) << "VB-Cable is currently the default playback device, switching to alternative...";
    collection_t collection;
    if (FAILED(device_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
      BOOST_LOG(error) << "Failed to enumerate audio endpoints";
      return -1;
    }

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
      device_t device;
      if (FAILED(collection->Item(i, &device))) {
        continue;
      }

      wstring_t device_id;
      if (FAILED(device->GetId(&device_id))) {
        continue;
      }

      if (device_id.get() != vb_output->second) {
        if (SUCCEEDED(set_default_device_all_roles(device_id.get()))) {
          BOOST_LOG(info) << "Successfully changed default playback device to: " << platf::to_utf8(device_id.get());
          // restoration_state.output_device_changed = true;
          BOOST_LOG(info) << "VB-Cable virtual microphone loopback successfully configured";
          return 0;
        }
      }
    }

    BOOST_LOG(error) << "No alternative playback device available";
    return -1;
  }

  void
  mic_write_wasapi_t::store_original_audio_settings() {
    if (restoration_state.settings_stored) {
      return;
    }

    if (!device_enum) {
      BOOST_LOG(warning) << "Device enumerator not available, skipping audio settings storage";
      return;
    }

    // 获取并存储当前默认输入设备ID
    device_t default_input;
    if (SUCCEEDED(device_enum->GetDefaultAudioEndpoint(eCapture, eConsole, &default_input)) && default_input) {
      wstring_t device_id;
      if (SUCCEEDED(default_input->GetId(&device_id))) {
        restoration_state.original_input_device_id = device_id.get();
        BOOST_LOG(debug) << "已存储原始输入设备: " << platf::to_utf8(restoration_state.original_input_device_id);
      }
      else {
        BOOST_LOG(warning) << "获取输入设备ID失败";
      }
    }
    else {
      BOOST_LOG(warning) << "获取默认输入设备失败";
    }

    restoration_state.settings_stored = true;
    BOOST_LOG(info) << "原始音频设备设置存储完成";
  }

  int
  mic_write_wasapi_t::restore_audio_devices() {
    if (!restoration_state.settings_stored) {
      BOOST_LOG(debug) << "No audio device settings to restore";
      return 0;
    }

    BOOST_LOG(info) << "Restoring audio devices to original state";

    int result = 0;

    // 恢复输入设备
    if (restoration_state.input_device_changed) {
      if (restore_original_input_device() != 0) {
        result = -1;
      }
    }

    // 重置恢复状态
    restoration_state.input_device_changed = false;
    restoration_state.settings_stored = false;

    BOOST_LOG(info) << "Audio device restoration " << (result == 0 ? "completed successfully" : "completed with errors");
    return result;
  }

  int
  mic_write_wasapi_t::restore_original_input_device() {
    if (restoration_state.original_input_device_id.empty()) {
      BOOST_LOG(warning) << "No original input device ID stored";
      return -1;
    }

    BOOST_LOG(info) << "Restoring original input device: " << platf::to_utf8(restoration_state.original_input_device_id);

    HRESULT hr = set_default_device_all_roles(restoration_state.original_input_device_id);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to restore original input device: [0x" << util::hex(hr).to_string_view() << "]";
      return -1;
    }

    BOOST_LOG(info) << "Successfully restored original input device";
    return 0;
  }

}  // namespace platf::audio
