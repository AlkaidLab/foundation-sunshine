/**
 * @file src/platform/windows/display_wgc.cpp
 * @brief Definitions for WinRT Windows.Graphics.Capture API
 */
// platform includes
#include <winsock2.h>
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <dxgi1_2.h>
#include <thread>

// local includes
#include "display.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/process.h"
#include <boost/program_options/parsers.hpp>
#include <vector>

// Gross hack to work around MINGW-packages#22160
#define ____FIReference_1_boolean_INTERFACE_DEFINED__

#include <Windows.Graphics.Capture.Interop.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.foundation.metadata.h>
#include <winrt/windows.graphics.directx.direct3d11.h>

namespace platf {
  using namespace std::literals;
}

namespace winrt {
  using namespace Windows::Foundation;
  using namespace Windows::Foundation::Metadata;
  using namespace Windows::Graphics::Capture;
  using namespace Windows::Graphics::DirectX::Direct3D11;

  extern "C" {
  HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(::IDXGIDevice *dxgiDevice, ::IInspectable **graphicsDevice);
  }

  /**
   * Windows structures sometimes have compile-time GUIDs. GCC supports this, but in a roundabout way.
   * If WINRT_IMPL_HAS_DECLSPEC_UUID is true, then the compiler supports adding this attribute to a struct. For example, Visual Studio.
   * If not, then MinGW GCC has a workaround to assign a GUID to a structure.
   */
  struct
#if WINRT_IMPL_HAS_DECLSPEC_UUID
    __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
#endif
    IDirect3DDxgiInterfaceAccess: ::IUnknown {
    virtual HRESULT __stdcall GetInterface(REFIID id, void **object) = 0;
  };
}  // namespace winrt
#if !WINRT_IMPL_HAS_DECLSPEC_UUID
static constexpr GUID GUID__IDirect3DDxgiInterfaceAccess = {
  0xA9B3D012,
  0x3DF2,
  0x4EE3,
  { 0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1 }
  // compare with __declspec(uuid(...)) for the struct above.
};

template <>
constexpr auto
__mingw_uuidof<winrt::IDirect3DDxgiInterfaceAccess>() -> GUID const & {
  return GUID__IDirect3DDxgiInterfaceAccess;
}
#endif

namespace platf::dxgi {
  /**
   * @brief Find a window by title (case-insensitive fuzzy matching).
   * @param window_title The window title to search for.
   * @return HWND of the found window, or nullptr if not found.
   * 
   * @note This function uses multiple matching strategies:
   *       1. Direct substring match
   *       2. Match after removing spaces
   *       3. Word-based match (all search words appear in window title)
   *       4. Fuzzy character sequence match (characters appear in order)
   *       It skips invisible windows and windows without titles.
   */
  static HWND
  find_window_by_title(const std::string &window_title) {
    if (window_title.empty()) {
      return nullptr;
    }

    std::wstring search_title = platf::from_utf8(window_title);

    // Convert to lowercase for case-insensitive comparison
    std::transform(search_title.begin(), search_title.end(), search_title.begin(), ::towlower);

    // Split search title into words for word-based matching
    std::vector<std::wstring> search_words = platf::split_words(search_title);
    std::wstring search_title_no_spaces = search_title;
    search_title_no_spaces.erase(std::remove(search_title_no_spaces.begin(), search_title_no_spaces.end(), L' '), search_title_no_spaces.end());

    struct EnumData {
      std::wstring search_title;
      std::wstring search_title_no_spaces;
      std::vector<std::wstring> search_words;
      HWND found_hwnd;
      HWND best_match_hwnd;  // Best fuzzy match found so far
      int best_match_score;  // Higher is better
    } enum_data { search_title, search_title_no_spaces, search_words, nullptr, nullptr, 0 };

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
      auto *data = reinterpret_cast<EnumData *>(lParam);

      // Check if window is visible
      if (!IsWindowVisible(hwnd)) {
        return TRUE;  // Continue enumeration
      }

      // Skip minimized windows as they may not capture properly
      if (IsIconic(hwnd)) {
        return TRUE;  // Continue enumeration
      }

      // Get window title length first
      int title_length = GetWindowTextLengthW(hwnd);
      if (title_length == 0) {
        return TRUE;  // Continue enumeration - no title
      }

      // Get window title
      std::wstring window_text(title_length + 1, L'\0');
      if (GetWindowTextW(hwnd, &window_text[0], title_length + 1) == 0) {
        return TRUE;  // Continue enumeration
      }
      window_text.resize(title_length);  // Remove null terminator from string

      // Convert to lowercase for case-insensitive comparison
      std::transform(window_text.begin(), window_text.end(), window_text.begin(), ::towlower);

      // Strategy 1: Direct substring match (highest priority)
      if (window_text.find(data->search_title) != std::wstring::npos) {
        data->found_hwnd = hwnd;
        return FALSE;  // Stop enumeration - exact match found
      }

      // Strategy 2: Match after removing spaces
      std::wstring window_text_no_spaces = window_text;
      window_text_no_spaces.erase(std::remove(window_text_no_spaces.begin(), window_text_no_spaces.end(), L' '), window_text_no_spaces.end());
      if (window_text_no_spaces.find(data->search_title_no_spaces) != std::wstring::npos) {
        data->found_hwnd = hwnd;
        return FALSE;  // Stop enumeration - good match found
      }

      // Strategy 3: Word-based matching (check if all search words appear in window title)
      if (!data->search_words.empty()) {
        bool all_words_found = true;
        for (const auto &word : data->search_words) {
          if (word.length() < 2) {
            continue;  // Skip very short words
          }
          if (window_text.find(word) == std::wstring::npos && 
              window_text_no_spaces.find(word) == std::wstring::npos) {
            all_words_found = false;
            break;
          }
        }
        if (all_words_found && data->search_words.size() > 0) {
          // Calculate a simple score based on word matches
          int score = static_cast<int>(data->search_words.size() * 10);
          if (score > data->best_match_score) {
            data->best_match_hwnd = hwnd;
            data->best_match_score = score;
          }
        }
      }

      // Strategy 4: Fuzzy character sequence matching (lowest priority, but still useful)
      if (platf::fuzzy_match(window_text, data->search_title)) {
        // Calculate score based on how close the match is
        int score = static_cast<int>(data->search_title.length() * 5);
        if (score > data->best_match_score) {
          data->best_match_hwnd = hwnd;
          data->best_match_score = score;
        }
      }

      return TRUE;  // Continue enumeration
    },
      reinterpret_cast<LPARAM>(&enum_data));

    // If exact match found, return it
    if (enum_data.found_hwnd != nullptr) {
      return enum_data.found_hwnd;
    }

    // Otherwise, return the best fuzzy match if we found one
    if (enum_data.best_match_hwnd != nullptr && enum_data.best_match_score > 0) {
      wchar_t actual_title[256] = { 0 };
      GetWindowTextW(enum_data.best_match_hwnd, actual_title, sizeof(actual_title) / sizeof(actual_title[0]));
      BOOST_LOG(debug) << "Using fuzzy match: ["sv << platf::to_utf8(actual_title) << "] for search ["sv << window_title << "] (score: "sv << enum_data.best_match_score << ")";
      return enum_data.best_match_hwnd;
    }

    return nullptr;
  }

  wgc_capture_t::wgc_capture_t() {
    InitializeConditionVariable(&frame_present_cv);
  }

  wgc_capture_t::~wgc_capture_t() {
    if (capture_session) {
      capture_session.Close();
    }
    if (frame_pool) {
      frame_pool.Close();
    }
    item = nullptr;
    capture_session = nullptr;
    frame_pool = nullptr;
  }

  /**
   * @brief Initialize the Windows.Graphics.Capture backend.
   * @return 0 on success, -1 on failure.
   */
  int
  wgc_capture_t::init(display_base_t *display, const ::video::config_t &config) {
    if (!winrt::GraphicsCaptureSession::IsSupported()) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows!"sv;
      return -1;
    }

    HRESULT status;
    dxgi::dxgi_t dxgi;
    winrt::com_ptr<::IInspectable> d3d_comhandle;

    if (FAILED(status = display->device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi))) {
      BOOST_LOG(error) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    try {
      if (FAILED(status = winrt::CreateDirect3D11DeviceFromDXGIDevice(*&dxgi, d3d_comhandle.put()))) {
        BOOST_LOG(error) << "Failed to query WinRT DirectX interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire device: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }

    uwp_device = d3d_comhandle.as<winrt::IDirect3DDevice>();

    auto capture_factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    if (capture_factory == nullptr) {
      BOOST_LOG(error) << "Failed to get GraphicsCaptureItem factory"sv;
      return -1;
    }

    // Determine capture target: window or display
    bool capture_window = false;
    std::string window_title;

    // Priority 1: Check capture_target configuration from global config
    if (config::video.capture_target == "window") {
      capture_window = true;
      window_title = config::video.window_title;
      desired_window_title = window_title;

      // If window_title is empty, try to derive from running app
      if (window_title.empty()) {
        int running_app_id = proc::proc.running();
        if (running_app_id > 0) {
          // Try to extract from the app command (executable path)
          std::string app_cmd = proc::proc.get_app_cmd(running_app_id);
          if (!app_cmd.empty()) {
            std::vector<std::string> parts;
            try {
              parts = boost::program_options::split_winmain(app_cmd);
            } catch (...) {
              // Ignore parsing errors
            }
            
            if (!parts.empty() && parts[0].find("://") == std::string::npos) {
              std::string exe_path = parts[0];
              size_t last_slash = exe_path.find_last_of("/\\");
              std::string filename = (last_slash != std::string::npos) ? exe_path.substr(last_slash + 1) : exe_path;
              size_t last_dot = filename.find_last_of('.');
              window_title = (last_dot != std::string::npos) ? filename.substr(0, last_dot) : filename;
              
              if (!window_title.empty()) {
                BOOST_LOG(info) << "Window title not specified, using executable filename: ["sv << window_title << "] (from: ["sv << app_cmd << "])";
              }
            }
          }
          
          // Fallback to app name if still empty
          if (window_title.empty()) {
            window_title = proc::proc.get_app_name(running_app_id);
            if (!window_title.empty()) {
              BOOST_LOG(info) << "Window title not specified, using app name: ["sv << window_title << "]";
            }
          }
          
          if (!window_title.empty()) {
            desired_window_title = window_title;
          }
        }
      }
    }
    // Priority 2: Check "window:" prefix in display_name (backward compatibility)
    else if (config.display_name.length() > 7 && config.display_name.substr(0, 7) == "window:") {
      capture_window = true;
      window_title = config.display_name.substr(7);
      desired_window_title = window_title;
    }
    else {
      desired_window_title.clear();
    }

    if (capture_window) {
      if (window_title.empty()) {
        BOOST_LOG(warning) << "Window capture requested but window_title is empty and no app is running. Falling back to display capture."sv;
        capture_window = false;
      }
      else {
        // Retry window lookup with timeout - window might be starting up
        constexpr int max_retries = 20;
        constexpr int retry_interval_ms = 500;
        HWND target_hwnd = nullptr;
        
        for (int retry = 0; retry < max_retries; ++retry) {
          target_hwnd = find_window_by_title(window_title);
          if (target_hwnd && IsWindow(target_hwnd) && IsWindowVisible(target_hwnd) && !IsIconic(target_hwnd)) {
            break;
          }
          target_hwnd = nullptr;
          
          if (retry < max_retries - 1) {
            BOOST_LOG(info) << "Window not found yet: ["sv << window_title << "], retrying in "sv << retry_interval_ms << "ms ("sv << (retry + 1) << "/"sv << max_retries << ")..."sv;
            Sleep(retry_interval_ms);
          }
        }
        
        if (!target_hwnd) {
          BOOST_LOG(warning) << "Window not found or invalid after "sv << max_retries << " attempts: ["sv << window_title << "]. Falling back to display capture."sv;
          capture_window = false;
        }
        else {
          wchar_t actual_title[256] = {};
          GetWindowTextW(target_hwnd, actual_title, std::size(actual_title));
          BOOST_LOG(info) << "Capturing window: ["sv << platf::to_utf8(actual_title) << "] (searched for: ["sv << window_title << "])";

          // Maximize the window first for better capture experience
          if (!IsZoomed(target_hwnd)) {
            BOOST_LOG(info) << "Maximizing window for capture..."sv;
            ShowWindow(target_hwnd, SW_MAXIMIZE);
            Sleep(500);
          }
          
          SetForegroundWindow(target_hwnd);
          Sleep(100);

          if (FAILED(status = capture_factory->CreateForWindow(target_hwnd, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item)))) {
            BOOST_LOG(error) << "Failed to create capture item for window [0x"sv << util::hex(status).to_string_view() << ']';
            return -1;
          }
          
          captured_window_hwnd = target_hwnd;
          
          auto window_size = item.Size();
          window_capture_width = static_cast<int>(window_size.Width);
          window_capture_height = static_cast<int>(window_size.Height);
          
          // Log window details for debugging
          RECT window_rect = {}, client_rect = {};
          if (GetWindowRect(target_hwnd, &window_rect) && GetClientRect(target_hwnd, &client_rect)) {
            BOOST_LOG(info) << "Window geometry - Window: "sv 
                            << (window_rect.right - window_rect.left) << 'x' << (window_rect.bottom - window_rect.top)
                            << ", Client: "sv << (client_rect.right - client_rect.left) << 'x' << (client_rect.bottom - client_rect.top)
                            << ", WGC initial: "sv << window_capture_width << 'x' << window_capture_height
                            << ", Display: "sv << display->width << 'x' << display->height;
          }
          
          BOOST_LOG(info) << "Window capture initialized with size: "sv << window_capture_width << 'x' << window_capture_height;
        }
      }
    }

    // If not capturing window (either not requested or fallback), capture display
    if (!capture_window) {
      captured_window_hwnd = nullptr;  // Not capturing a window
      window_capture_width = 0;
      window_capture_height = 0;
      if (display->output == nullptr) {
        BOOST_LOG(error) << "Display output is null, cannot capture monitor"sv;
        return -1;
      }
      DXGI_OUTPUT_DESC output_desc;
      display->output->GetDesc(&output_desc);
      BOOST_LOG(info) << "Capturing display: ["sv << platf::to_utf8(output_desc.DeviceName) << ']';
      if (FAILED(status = capture_factory->CreateForMonitor(output_desc.Monitor, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item)))) {
        BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire display: [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    }

    display->capture_format = config.dynamicRange ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;

    // Use the actual capture item size for frame pool creation
    auto item_size = item.Size();
    
    try {
      frame_pool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(uwp_device, static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(display->capture_format), 2, item_size);
      capture_session = frame_pool.CreateCaptureSession(item);
      frame_pool.FrameArrived({ this, &wgc_capture_t::on_frame_arrived });
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to create capture session: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }

    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
        capture_session.IsBorderRequired(false);
      }
      else {
        BOOST_LOG(warning) << "Can't disable colored border around capture area on this version of Windows";
      }
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Screen capture may not be fully supported on this device for this release of Windows: failed to disable border around capture area: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }

    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
        capture_session.MinUpdateInterval(4ms);
      }
      else {
        BOOST_LOG(warning) << "Can't set MinUpdateInterval on this version of Windows";
      }
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Screen capture may be capped to 60fps on this device for this release of Windows: failed to set MinUpdateInterval: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }

    try {
      capture_session.StartCapture();
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to start capture: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }

    return 0;
  }

  /**
   * This function runs in a separate thread spawned by the frame pool and is a producer of frames.
   * To maintain parity with the original display interface, this frame will be consumed by the capture thread.
   * Acquire a read-write lock, make the produced frame available to the capture thread, then wake the capture thread.
   */
  void
  wgc_capture_t::on_frame_arrived(winrt::Direct3D11CaptureFramePool const &sender, winrt::IInspectable const &) {
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame { nullptr };
    try {
      frame = sender.TryGetNextFrame();
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Failed to capture frame: "sv << e.code();
      return;
    }
    if (frame != nullptr) {
      AcquireSRWLockExclusive(&frame_lock);
      if (produced_frame) {
        produced_frame.Close();
      }

      produced_frame = frame;
      ReleaseSRWLockExclusive(&frame_lock);
      WakeConditionVariable(&frame_present_cv);
    }
  }

  /**
   * @brief Get the next frame from the producer thread.
   * If not available, the capture thread blocks until one is, or the wait times out.
   * @param timeout how long to wait for the next frame
   * @param out a texture containing the frame just captured
   * @param out_time the timestamp of the frame just captured
   */
  capture_e
  wgc_capture_t::next_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out, uint64_t &out_time) {
    // this CONSUMER runs in the capture thread
    release_frame();

    AcquireSRWLockExclusive(&frame_lock);
    if (produced_frame == nullptr && SleepConditionVariableSRW(&frame_present_cv, &frame_lock, timeout.count(), 0) == 0) {
      ReleaseSRWLockExclusive(&frame_lock);
      if (GetLastError() == ERROR_TIMEOUT) {
        return capture_e::timeout;
      }
      else {
        return capture_e::error;
      }
    }
    if (produced_frame) {
      consumed_frame = produced_frame;
      produced_frame = nullptr;
    }
    ReleaseSRWLockExclusive(&frame_lock);
    if (consumed_frame == nullptr) {  // spurious wakeup
      return capture_e::timeout;
    }

    auto capture_access = consumed_frame.Surface().as<winrt::IDirect3DDxgiInterfaceAccess>();
    if (capture_access == nullptr) {
      return capture_e::error;
    }
    capture_access->GetInterface(IID_ID3D11Texture2D, (void **) out);
    out_time = consumed_frame.SystemRelativeTime().count();  // raw ticks from query performance counter
    return capture_e::ok;
  }

  capture_e
  wgc_capture_t::release_frame() {
    if (consumed_frame != nullptr) {
      consumed_frame.Close();
      consumed_frame = nullptr;
    }
    return capture_e::ok;
  }

  int
  wgc_capture_t::set_cursor_visible(bool x) {
    try {
      if (capture_session.IsCursorCaptureEnabled() != x) {
        capture_session.IsCursorCaptureEnabled(x);
      }
      return 0;
    }
    catch (winrt::hresult_error &) {
      return -1;
    }
  }

  bool
  wgc_capture_t::is_window_valid() const {
    // If not capturing a window, always return true
    if (captured_window_hwnd == nullptr) {
      return true;
    }
    // Check if window is still valid
    if (IsWindow(captured_window_hwnd) == FALSE) {
      return false;
    }
    // Check if window is minimized - minimized windows may not capture properly
    if (IsIconic(captured_window_hwnd) != FALSE) {
      return false;
    }
    // Check if window is visible - invisible windows may not capture properly
    if (IsWindowVisible(captured_window_hwnd) == FALSE) {
      return false;
    }
    return true;
  }

  std::shared_ptr<img_t>
  display_wgc_ram_t::alloc_img() {
    auto img = std::make_shared<img_t>();
    
    // For window capture, use window capture dimensions; for display capture, use display dimensions
    int img_width = dup.window_capture_width > 0 ? dup.window_capture_width : width;
    int img_height = dup.window_capture_height > 0 ? dup.window_capture_height : height;
    
    img->width = img_width;
    img->height = img_height;
    img->pixel_pitch = get_pixel_pitch();
    img->row_pitch = img->pixel_pitch * img->width;
    img->data = nullptr;
    
    return img;
  }

  int
  display_wgc_ram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    texture.reset();
    return 0;
  }

  /**
   * @brief Get the next frame from the Windows.Graphics.Capture API and copy it into a new snapshot texture.
   * @param pull_free_image_cb call this to get a new free image from the video subsystem.
   * @param img_out the captured frame is returned here
   * @param timeout how long to wait for the next frame
   * @param cursor_visible whether to capture the cursor
   */
  capture_e
  display_wgc_ram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    // Check if window is still valid (if capturing a window)
    if (!dup.is_window_valid()) {
      BOOST_LOG(warning) << "Captured window is no longer valid, reinitializing capture"sv;
      return capture_e::reinit;
    }
    
    HRESULT status;
    texture2d_t src;
    uint64_t frame_qpc;
    dup.set_cursor_visible(cursor_visible);
    auto capture_status = dup.next_frame(timeout, &src, frame_qpc);
    if (capture_status != capture_e::ok) {
      // If we're capturing a window and getting timeouts/errors, check if window is still valid
      if (dup.captured_window_hwnd != nullptr) {
        // Simplified: Any error or timeout means window might have changed, check validity
        if (!dup.is_window_valid()) {
          BOOST_LOG(warning) << "Captured window is no longer valid, reinitializing capture"sv;
          return capture_e::reinit;
        }
      }
      return capture_status;
    }

    auto frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), frame_qpc);
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);

    // Get the actual captured frame dimensions
    int frame_width = static_cast<int>(desc.Width);
    int frame_height = static_cast<int>(desc.Height);
    
    // For window capture, update stored dimensions if they changed
    if (dup.captured_window_hwnd != nullptr) {
      if (dup.window_capture_width != frame_width || dup.window_capture_height != frame_height) {
        BOOST_LOG(info) << "Window capture size changed: "sv << dup.window_capture_width << 'x' << dup.window_capture_height 
                         << " -> "sv << frame_width << 'x' << frame_height;
        dup.window_capture_width = frame_width;
        dup.window_capture_height = frame_height;
        // Reset texture to force recreation with new size
        texture.reset();
      }
    }

    // Create the staging texture if it doesn't exist. It should match the source in size and format.
    if (texture == nullptr) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "Capture format ["sv << dxgi_format_to_string(capture_format) << ']';
      BOOST_LOG(info) << "Creating staging texture: "sv << frame_width << 'x' << frame_height;

      D3D11_TEXTURE2D_DESC t {};
      t.Width = frame_width;
      t.Height = frame_height;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_STAGING;
      t.Format = capture_format;
      t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      auto status = device->CreateTexture2D(&t, nullptr, &texture);

      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create staging texture [0x"sv << util::hex(status).to_string_view() << ']';
        return capture_e::error;
      }
    }

    // Check if the captured frame size matches our staging texture
    D3D11_TEXTURE2D_DESC staging_desc;
    texture->GetDesc(&staging_desc);
    
    if (frame_width != static_cast<int>(staging_desc.Width) || frame_height != static_cast<int>(staging_desc.Height)) {
      BOOST_LOG(info) << "Capture size mismatch - frame: "sv << frame_width << 'x' << frame_height 
                       << ", staging: "sv << staging_desc.Width << 'x' << staging_desc.Height
                       << ", recreating staging texture"sv;
      // Reset texture to force recreation with new size on next iteration
      texture.reset();
      // Don't reinit the whole capture, just recreate the staging texture next frame
      return capture_e::timeout;
    }
    
    // It's also possible for the capture format to change on the fly. If that happens,
    // reinitialize capture to try format detection again and create new images.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    // Copy from GPU to CPU
    device_ctx->CopyResource(texture.get(), src.get());

    if (!pull_free_image_cb(img_out)) {
      return capture_e::interrupted;
    }
    auto img = (img_t *) img_out.get();

    // Map the staging texture for CPU access (making it inaccessible for the GPU)
    if (FAILED(status = device_ctx->Map(texture.get(), 0, D3D11_MAP_READ, 0, &img_info))) {
      BOOST_LOG(error) << "Failed to map texture [0x"sv << util::hex(status).to_string_view() << ']';

      return capture_e::error;
    }

    // Now that we know the capture format, we can finish creating the image
    if (complete_img(img, false)) {
      device_ctx->Unmap(texture.get(), 0);
      img_info.pData = nullptr;
      return capture_e::error;
    }

    // Copy data using actual frame dimensions
    std::copy_n((std::uint8_t *) img_info.pData, frame_height * img_info.RowPitch, (std::uint8_t *) img->data);

    // Unmap the staging texture to allow GPU access again
    device_ctx->Unmap(texture.get(), 0);
    img_info.pData = nullptr;

    if (img) {
      img->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e
  display_wgc_ram_t::release_snapshot() {
    return dup.release_frame();
  }
}  // namespace platf::dxgi
