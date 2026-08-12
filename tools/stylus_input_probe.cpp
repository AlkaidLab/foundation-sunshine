#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0602
#endif

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <numbers>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {
  constexpr wchar_t window_class_name[] = L"SunshineStylusInputProbe";
  constexpr wchar_t window_title[] = L"Sunshine 手写笔输入检测";

  constexpr UINT_PTR report_timer_id = 1;
  constexpr UINT_PTR repaint_timer_id = 2;
  constexpr UINT report_interval_ms = 5000;
  constexpr UINT repaint_interval_ms = 16;
  constexpr std::size_t max_trace_points = 4096;
  constexpr std::size_t max_graph_samples = 160;
  constexpr std::size_t max_data_samples = 200000;
  constexpr std::size_t recording_checkpoint_samples = 256;
  constexpr std::uintmax_t max_data_file_bytes = 64ULL * 1024ULL * 1024ULL;
  constexpr UINT32 max_pointer_history_samples = 512;
  constexpr UINT32 pen_pressure_max = 1024;

  constexpr int clear_button_id = 1001;
  constexpr int copy_button_id = 1002;
  constexpr int open_log_button_id = 1003;
  constexpr int filter_promoted_mouse_checkbox_id = 1004;
  constexpr int import_data_button_id = 1005;
  constexpr int export_data_button_id = 1006;
  constexpr int record_data_button_id = 1007;
  constexpr int copy_log_path_button_id = 1008;
  constexpr int open_recording_folder_button_id = 1009;

  constexpr std::string_view stylus_data_magic = "SUNSHINE_STYLUS_DAT\t1";
  constexpr std::uint8_t stylus_event_hover = 0x00;
  constexpr std::uint8_t stylus_event_down = 0x01;
  constexpr std::uint8_t stylus_event_up = 0x02;
  constexpr std::uint8_t stylus_event_move = 0x03;
  constexpr std::uint8_t stylus_event_cancel = 0x04;
  constexpr std::uint8_t stylus_event_button_only = 0x05;
  constexpr std::uint8_t stylus_event_hover_leave = 0x06;
  constexpr std::uint8_t stylus_event_cancel_all = 0x07;
  constexpr std::uint32_t stylus_rotation_unknown = 0xFFFF;
  constexpr std::int32_t stylus_tilt_unknown = 0xFF;

  constexpr std::uint32_t mouse_pointer_signature = 0xFF515700u;
  constexpr std::uint32_t mouse_pointer_signature_mask = 0xFFFFFF00u;
  constexpr std::uint32_t mouse_pointer_touch_flag = 0x00000080u;

  struct trace_point_t {
    POINT point {};
    bool break_before {false};
    UINT32 pressure {};
    bool pressure_available {false};
    UINT32 pointer_time {};
    UINT64 performance_count {};
  };

  struct sampling_analysis_t {
    std::vector<double> recent_intervals_ms;
    std::size_t point_count {};
    double interval_median_ms {};
    double interval_p95_ms {};
    double interval_max_ms {};
    double turn_median_degrees {};
    double turn_p95_degrees {};
  };

  struct imported_pen_sample_t {
    std::uint64_t timestamp_us {};
    std::uint8_t event_type {};
    double x {};
    double y {};
    double pressure {};
    std::uint32_t rotation {};
    std::int32_t tilt {};
  };

  struct imported_pen_data_t {
    std::vector<imported_pen_sample_t> samples;
    bool truncated {};
  };

  class log_writer_t {
  public:
    /** Open the per-run status log. */
    bool
    open(const std::filesystem::path &path) noexcept {
      try {
        stream_.open(path, std::ios::binary | std::ios::trunc);
        if (!stream_) {
          return false;
        }

        constexpr unsigned char utf8_bom[] {0xEF, 0xBB, 0xBF};
        stream_.write(reinterpret_cast<const char *>(utf8_bom), sizeof(utf8_bom));
        stream_.flush();
        return static_cast<bool>(stream_);
      }
      catch (...) {
        stream_.close();
        return false;
      }
    }

    /** Append one low-frequency status line. */
    void
    write(const std::string &line) noexcept {
      try {
        if (!stream_) {
          return;
        }
        stream_.write(line.data(), static_cast<std::streamsize>(line.size()));
        stream_.write("\r\n", 2);
        stream_.flush();
      }
      catch (...) {
        stream_.close();
      }
    }

    /** Flush the status log without propagating filesystem errors. */
    void
    flush() noexcept {
      try {
        if (stream_) {
          stream_.flush();
        }
      }
      catch (...) {
        stream_.close();
      }
    }

  private:
    std::ofstream stream_;
  };

  struct interval_stats_t {
    std::uint64_t pen_down {};
    std::uint64_t pen_update {};
    std::uint64_t pen_up {};
    std::uint64_t pen_hover {};
    std::uint64_t pen_errors {};
    std::uint64_t mouse_down {};
    std::uint64_t mouse_move {};
    std::uint64_t mouse_up {};
    std::uint64_t promoted_mouse {};
    std::uint64_t correlated_mouse {};
    std::uint64_t filtered_promoted_mouse {};

    bool pressure_seen {false};
    UINT32 min_pressure {std::numeric_limits<UINT32>::max()};
    UINT32 max_pressure {};
    bool tilt_seen {false};
    INT32 min_tilt_x {std::numeric_limits<INT32>::max()};
    INT32 max_tilt_x {std::numeric_limits<INT32>::min()};
    INT32 min_tilt_y {std::numeric_limits<INT32>::max()};
    INT32 max_tilt_y {std::numeric_limits<INT32>::min()};
    bool rotation_seen {false};
    UINT32 min_rotation {std::numeric_limits<UINT32>::max()};
    UINT32 max_rotation {};
  };

  struct app_state_t {
    HWND clear_button {};
    HWND copy_button {};
    HWND open_log_button {};
    HWND copy_log_path_button {};
    HWND open_recording_folder_button {};
    HWND import_data_button {};
    HWND export_data_button {};
    HWND record_data_button {};
    HWND filter_promoted_mouse_checkbox {};
    HFONT ui_font {};
    UINT dpi {USER_DEFAULT_SCREEN_DPI};
    RECT canvas {};

    std::deque<trace_point_t> pen_trace;
    std::deque<trace_point_t> mouse_trace;
    sampling_analysis_t sampling_analysis;
    interval_stats_t stats;

    bool pen_in_contact {false};
    bool pen_trace_break_pending {false};
    bool mouse_in_contact {false};
    bool pen_event_seen {false};
    bool mouse_event_seen {false};
    POINT last_pen_point {};
    POINT last_mouse_point {};
    bool last_mouse_point_available {false};
    ULONGLONG last_pen_tick {};
    UINT32 last_pointer_id {};
    UINT32 last_pressure {};
    bool last_pressure_available {false};
    INT32 last_tilt_x {};
    INT32 last_tilt_y {};
    bool last_tilt_available {false};
    UINT32 last_rotation {};
    bool last_rotation_available {false};

    std::wstring last_report {L"等待输入。请在白色画布内按下后用手写笔连续绘制。"};
    std::wstring last_event {L"尚未收到检测区输入事件。"};
    std::filesystem::path log_path;
    std::filesystem::path recording_directory;
    log_writer_t log_writer;
    imported_pen_data_t pen_data;
    std::filesystem::path recording_path;
    std::ofstream recording_stream;
    std::size_t recording_saved_samples {};
    bool recording_checkpoint_requested {};
    bool recording_truncation_written {};
    UINT64 capture_origin_performance_count {};
    UINT32 capture_origin_pointer_time {};
    bool capture_time_origin_set {};
    bool recording {};
    bool fatal_error {};
    bool pen_read_error_reported {};
    bool pen_history_error_reported {};
    bool pen_history_limit_reported {};
    bool pen_history_allocation_error_reported {};
    bool filter_promoted_mouse {};
    bool viewing_imported_data {};
    bool canvas_dirty {true};
    bool sampling_analysis_dirty {true};
  };

  void
  write_log_line(app_state_t &state, std::wstring_view line) noexcept;

  /** Enable per-monitor DPI handling before any window is created. */
  void
  enable_per_monitor_dpi_awareness() {
    using set_process_dpi_awareness_context_t = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
    if (const auto set_process_dpi_awareness_context = reinterpret_cast<set_process_dpi_awareness_context_t>(
          GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")
        );
        set_process_dpi_awareness_context != nullptr &&
        set_process_dpi_awareness_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
      return;
    }

    // Windows 8.1 通过 Shcore 提供按显示器 DPI 感知。动态加载可以兼容旧系统，
    // 同时避免增加新的链接依赖。
    if (const auto shcore = LoadLibraryW(L"shcore.dll"); shcore != nullptr) {
      using set_process_dpi_awareness_t = HRESULT(WINAPI *)(int);
      constexpr int process_per_monitor_dpi_aware = 2;
      const auto set_process_dpi_awareness = reinterpret_cast<set_process_dpi_awareness_t>(
        GetProcAddress(shcore, "SetProcessDpiAwareness")
      );
      const auto enabled = set_process_dpi_awareness != nullptr &&
                           SUCCEEDED(set_process_dpi_awareness(process_per_monitor_dpi_aware));
      FreeLibrary(shcore);
      if (enabled) {
        return;
      }
    }

    SetProcessDPIAware();
  }

  /** Scale a device-independent pixel value for the active monitor. */
  int
  scale_for_dpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
  }

  /** Read the effective DPI for a window with a Windows 8 fallback. */
  UINT
  get_window_dpi(HWND window) {
    using get_dpi_for_window_t = UINT(WINAPI *)(HWND);
    static const auto get_dpi_for_window = reinterpret_cast<get_dpi_for_window_t>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")
    );
    if (get_dpi_for_window != nullptr) {
      const auto dpi = get_dpi_for_window(window);
      if (dpi != 0) {
        return dpi;
      }
    }

    auto device_context = GetDC(window);
    if (device_context == nullptr) {
      return USER_DEFAULT_SCREEN_DPI;
    }

    const auto dpi = static_cast<UINT>(GetDeviceCaps(device_context, LOGPIXELSX));
    ReleaseDC(window, device_context);
    return dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
  }

  /** Read the system DPI before the first window is created. */
  UINT
  get_system_dpi() {
    auto device_context = GetDC(nullptr);
    if (device_context == nullptr) {
      return USER_DEFAULT_SCREEN_DPI;
    }

    const auto dpi = static_cast<UINT>(GetDeviceCaps(device_context, LOGPIXELSX));
    ReleaseDC(nullptr, device_context);
    return dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
  }

  /** Recreate the UI font for the active monitor. */
  void
  update_ui_font(app_state_t &state) {
    const auto old_font = state.ui_font;
    const auto new_font = CreateFontW(
      -scale_for_dpi(13, state.dpi),
      0,
      0,
      0,
      FW_NORMAL,
      FALSE,
      FALSE,
      FALSE,
      DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS,
      CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE,
      L"Segoe UI"
    );
    if (new_font != nullptr) {
      state.ui_font = new_font;
    }

    const auto font = reinterpret_cast<WPARAM>(state.ui_font != nullptr ? state.ui_font : GetStockObject(DEFAULT_GUI_FONT));
    if (state.clear_button != nullptr) {
      SendMessageW(state.clear_button, WM_SETFONT, font, TRUE);
    }
    if (state.copy_button != nullptr) {
      SendMessageW(state.copy_button, WM_SETFONT, font, TRUE);
    }
    if (state.open_log_button != nullptr) {
      SendMessageW(state.open_log_button, WM_SETFONT, font, TRUE);
    }
    if (state.copy_log_path_button != nullptr) {
      SendMessageW(state.copy_log_path_button, WM_SETFONT, font, TRUE);
    }
    if (state.open_recording_folder_button != nullptr) {
      SendMessageW(state.open_recording_folder_button, WM_SETFONT, font, TRUE);
    }
    if (state.import_data_button != nullptr) {
      SendMessageW(state.import_data_button, WM_SETFONT, font, TRUE);
    }
    if (state.export_data_button != nullptr) {
      SendMessageW(state.export_data_button, WM_SETFONT, font, TRUE);
    }
    if (state.record_data_button != nullptr) {
      SendMessageW(state.record_data_button, WM_SETFONT, font, TRUE);
    }
    if (state.filter_promoted_mouse_checkbox != nullptr) {
      SendMessageW(state.filter_promoted_mouse_checkbox, WM_SETFONT, font, TRUE);
    }
    if (new_font != nullptr && old_font != nullptr) {
      DeleteObject(old_font);
    }
  }

  /** Scale existing traces when the window moves between monitors. */
  void
  scale_recorded_points(app_state_t &state, UINT old_dpi, UINT new_dpi) {
    if (old_dpi == 0 || old_dpi == new_dpi) {
      return;
    }

    const auto scale_point = [old_dpi, new_dpi](POINT &point) {
      point.x = MulDiv(point.x, static_cast<int>(new_dpi), static_cast<int>(old_dpi));
      point.y = MulDiv(point.y, static_cast<int>(new_dpi), static_cast<int>(old_dpi));
    };
    for (auto &point : state.pen_trace) {
      scale_point(point.point);
    }
    for (auto &point : state.mouse_trace) {
      scale_point(point.point);
    }
    scale_point(state.last_pen_point);
  }

  /** Convert UTF-16 text to UTF-8. */
  std::string
  to_utf8(std::wstring_view value) {
    if (value.empty()) {
      return {};
    }

    const auto size = WideCharToMultiByte(
      CP_UTF8,
      0,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0,
      nullptr,
      nullptr
    );
    if (size <= 0) {
      return {};
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
      CP_UTF8,
      0,
      value.data(),
      static_cast<int>(value.size()),
      result.data(),
      size,
      nullptr,
      nullptr
    );
    return result;
  }

  /** Return a timestamped status log path inside the recording directory. */
  std::filesystem::path
  make_log_path(const std::filesystem::path &recording_directory) {
    SYSTEMTIME time {};
    GetLocalTime(&time);
    std::wostringstream file_name;
    file_name << L"sunshine-stylus-input-probe-"
              << std::setfill(L'0')
              << std::setw(4) << time.wYear
              << std::setw(2) << time.wMonth
              << std::setw(2) << time.wDay << L'-'
              << std::setw(2) << time.wHour
              << std::setw(2) << time.wMinute
              << std::setw(2) << time.wSecond << L'-'
              << GetCurrentProcessId() << L".log";
    return recording_directory / file_name.str();
  }

  /** Return the dedicated recording directory below the current Windows temporary directory. */
  std::filesystem::path
  make_recording_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    const auto length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (length != 0 && length < buffer.size()) {
      buffer.resize(length);
      return std::filesystem::path(buffer) / L"Sunshine" / L"stylus-input-probe" / L"recordings";
    }

    return std::filesystem::current_path() / L"Sunshine" / L"stylus-input-probe" / L"recordings";
  }

  /** Create the recording directory without allowing filesystem exceptions into the UI callback. */
  bool
  ensure_recording_directory(const std::filesystem::path &path, std::wstring &error) noexcept {
    try {
      std::error_code filesystem_error;
      std::filesystem::create_directories(path, filesystem_error);
      if (filesystem_error || !std::filesystem::is_directory(path, filesystem_error)) {
        error = L"无法创建录制目录。";
        return false;
      }
      error.clear();
      return true;
    }
    catch (...) {
      error = L"创建录制目录时发生异常。";
      return false;
    }
  }

  /** Read a versioned stylus data file. */
  bool
  read_stylus_data_impl(const std::filesystem::path &path, imported_pen_data_t &data, std::wstring &error) {
    data = {};
    error.clear();
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (!size_error && file_size > max_data_file_bytes) {
      error = L"数据文件超过 64 MiB 的读取上限。";
      return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
      error = L"无法打开数据文件。";
      return false;
    }

    std::string line;
    if (!std::getline(stream, line)) {
      error = L"数据文件为空。";
      return false;
    }
    if (line.ends_with('\r')) {
      line.pop_back();
    }
    if (line.starts_with("\xEF\xBB\xBF")) {
      line.erase(0, 3);
    }
    if (line != stylus_data_magic) {
      error = L"不是受支持的手写笔数据文件。";
      return false;
    }

    std::uint64_t previous_timestamp {};
    bool have_previous_timestamp = false;
    std::size_t line_number = 1;
    while (std::getline(stream, line)) {
      ++line_number;
      if (line.ends_with('\r')) {
        line.pop_back();
      }
      if (line.starts_with("# truncated=true")) {
        data.truncated = true;
        continue;
      }
      if (line.empty() || line.starts_with('#')) {
        continue;
      }

      char record_type {};
      unsigned int event_type {};
      unsigned int rotation {};
      int tilt {};
      imported_pen_sample_t sample;
      std::istringstream record(line);
      record.imbue(std::locale::classic());
      if (!(record >> record_type >> sample.timestamp_us >> event_type >> sample.x >> sample.y >>
            sample.pressure >> rotation >> tilt) ||
          record_type != 'P') {
        error = L"第 " + std::to_wstring(line_number) + L" 行格式无效。";
        return false;
      }
      std::string trailing;
      if (record >> trailing) {
        error = L"第 " + std::to_wstring(line_number) + L" 行包含多余字段。";
        return false;
      }
      if (event_type > 7 ||
          !std::isfinite(sample.x) || sample.x < 0.0 || sample.x > 1.0 ||
          !std::isfinite(sample.y) || sample.y < 0.0 || sample.y > 1.0 ||
          !std::isfinite(sample.pressure) || sample.pressure < 0.0 || sample.pressure > 1.0 ||
          (rotation > 359 && rotation != stylus_rotation_unknown) ||
          (tilt < 0 || (tilt > 90 && tilt != stylus_tilt_unknown))) {
        error = L"第 " + std::to_wstring(line_number) + L" 行的数据超出有效范围。";
        return false;
      }
      if (have_previous_timestamp && sample.timestamp_us < previous_timestamp) {
        error = L"第 " + std::to_wstring(line_number) + L" 行的时间戳发生倒退。";
        return false;
      }
      if (data.samples.size() >= max_data_samples) {
        error = L"数据文件超过 200000 个样本的导入上限。";
        return false;
      }

      sample.event_type = static_cast<std::uint8_t>(event_type);
      sample.rotation = rotation;
      sample.tilt = tilt;
      data.samples.push_back(sample);
      previous_timestamp = sample.timestamp_us;
      have_previous_timestamp = true;
    }

    if (stream.bad()) {
      error = L"读取数据文件时发生错误。";
      return false;
    }
    if (data.samples.empty()) {
      error = L"数据文件不包含手写笔样本。";
      return false;
    }
    return true;
  }

  /** Read a data file without allowing allocation or filesystem exceptions into the window procedure. */
  bool
  read_stylus_data(const std::filesystem::path &path, imported_pen_data_t &data, std::wstring &error) {
    try {
      return read_stylus_data_impl(path, data, error);
    }
    catch (const std::exception &) {
      data = {};
      error = L"读取数据文件时发生异常。";
      return false;
    }
    catch (...) {
      data = {};
      error = L"读取数据文件时发生未知异常。";
      return false;
    }
  }

  /** Write the header shared by exported and incrementally recorded data files. */
  void
  write_stylus_data_header(std::ostream &stream) {
    stream << stylus_data_magic << '\n'
           << "# columns=P timestamp_us event_type x y pressure rotation tilt\n";
    stream << std::setprecision(std::numeric_limits<double>::max_digits10);
  }

  /** Write one versioned stylus data record. */
  void
  write_stylus_data_sample(std::ostream &stream, const imported_pen_sample_t &sample) {
    stream << "P "
           << sample.timestamp_us << ' '
           << static_cast<unsigned int>(sample.event_type) << ' '
           << sample.x << ' '
           << sample.y << ' '
           << sample.pressure << ' '
           << sample.rotation << ' '
           << sample.tilt << '\n';
  }

  /** Write a complete versioned stylus data file. */
  bool
  write_stylus_data_impl(const std::filesystem::path &path, const imported_pen_data_t &data, std::wstring &error) {
    error.clear();
    auto temporary_path = path;
    temporary_path += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L'-' + std::to_wstring(GetTickCount64());
    struct temporary_file_guard_t {
      const std::filesystem::path &path;
      bool active {true};

      ~temporary_file_guard_t() {
        if (active) {
          std::error_code remove_error;
          std::filesystem::remove(path, remove_error);
        }
      }
    } temporary_file_guard {temporary_path};

    std::ofstream stream(temporary_path, std::ios::binary | std::ios::trunc);
    if (!stream) {
      error = L"无法创建数据文件。";
      return false;
    }

    stream.imbue(std::locale::classic());
    write_stylus_data_header(stream);
    for (const auto &sample : data.samples) {
      write_stylus_data_sample(stream, sample);
    }
    if (data.truncated) {
      stream << "# truncated=true\n";
    }
    stream.flush();
    if (!stream) {
      error = L"写入数据文件时发生错误。";
      stream.close();
      return false;
    }
    stream.close();
    if (stream.fail()) {
      error = L"关闭数据文件时发生错误。";
      return false;
    }
    if (!MoveFileExW(temporary_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      error = L"替换目标数据文件时发生错误。";
      return false;
    }
    temporary_file_guard.active = false;
    return true;
  }

  /** Write a data file without allowing allocation or filesystem exceptions into the window procedure. */
  bool
  write_stylus_data(const std::filesystem::path &path, const imported_pen_data_t &data, std::wstring &error) {
    try {
      return write_stylus_data_impl(path, data, error);
    }
    catch (const std::exception &) {
      error = L"写入数据文件时发生异常。";
      return false;
    }
    catch (...) {
      error = L"写入数据文件时发生未知异常。";
      return false;
    }
  }

  /** Open an incremental recording file and persist its header immediately. */
  bool
  open_recording_file(app_state_t &state, const std::filesystem::path &path, std::wstring &error) noexcept {
    try {
      state.recording_stream.close();
      state.recording_stream.clear();
      state.recording_stream.open(path, std::ios::binary | std::ios::trunc);
      if (!state.recording_stream) {
        error = L"无法创建数据文件。";
        return false;
      }

      state.recording_stream.imbue(std::locale::classic());
      write_stylus_data_header(state.recording_stream);
      state.recording_stream.flush();
      if (!state.recording_stream) {
        error = L"写入数据文件头时发生错误。";
        state.recording_stream.close();
        return false;
      }

      state.recording_saved_samples = 0;
      state.recording_checkpoint_requested = false;
      state.recording_truncation_written = false;
      error.clear();
      return true;
    }
    catch (...) {
      state.recording_stream.close();
      error = L"创建数据文件时发生异常。";
      return false;
    }
  }

  /** Append samples not yet stored on disk and flush one bounded checkpoint. */
  bool
  checkpoint_recording_file(app_state_t &state, std::wstring &error) noexcept {
    try {
      if (!state.recording_stream.is_open()) {
        error = L"录制数据文件已关闭。";
        return false;
      }

      for (auto index = state.recording_saved_samples; index < state.pen_data.samples.size(); ++index) {
        write_stylus_data_sample(state.recording_stream, state.pen_data.samples[index]);
      }
      if (state.pen_data.truncated && !state.recording_truncation_written) {
        state.recording_stream << "# truncated=true\n";
        state.recording_truncation_written = true;
      }
      state.recording_stream.flush();
      if (!state.recording_stream) {
        error = L"写入录制数据时发生错误。";
        state.recording_stream.close();
        return false;
      }

      state.recording_saved_samples = state.pen_data.samples.size();
      state.recording_checkpoint_requested = false;
      error.clear();
      return true;
    }
    catch (...) {
      state.recording_stream.close();
      error = L"写入录制数据时发生异常。";
      return false;
    }
  }

  /** Close the incremental data file after persisting its final checkpoint. */
  bool
  close_recording_file(app_state_t &state, std::wstring &error) noexcept {
    auto saved = checkpoint_recording_file(state, error);
    state.recording_stream.close();
    if (saved && state.recording_stream.fail()) {
      error = L"关闭录制数据文件时发生错误。";
      saved = false;
    }
    return saved;
  }

  /** Append one bounded point to a trace. */
  void
  append_trace(std::deque<trace_point_t> &trace, trace_point_t point) {
    if (trace.size() >= max_trace_points) {
      trace.pop_front();
      if (!trace.empty()) {
        trace.front().break_before = true;
      }
    }
    trace.push_back(point);
  }

  /** Return a percentile from a small sample set without modifying the caller's data. */
  double
  percentile(std::vector<double> values, double quantile) {
    if (values.empty()) {
      return 0.0;
    }

    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::ceil(quantile * static_cast<double>(values.size()))) - 1;
    return values[std::min(index, values.size() - 1)];
  }

  /** Convert two host pointer timestamps into a delivery interval. */
  double
  sampling_interval_ms(const trace_point_t &previous, const trace_point_t &current, double performance_frequency) {
    if (performance_frequency > 0.0 && previous.performance_count != 0 &&
        current.performance_count >= previous.performance_count) {
      return static_cast<double>(current.performance_count - previous.performance_count) * 1000.0 / performance_frequency;
    }

    return static_cast<UINT32>(current.pointer_time - previous.pointer_time);
  }

  /** Analyze the recent host-delivered samples from the latest continuous pen stroke. */
  sampling_analysis_t
  analyze_sampling(const std::deque<trace_point_t> &trace) {
    sampling_analysis_t analysis;
    if (trace.size() < 2) {
      return analysis;
    }

    std::size_t stroke_start {};
    for (std::size_t index = 1; index < trace.size(); ++index) {
      if (trace[index].break_before) {
        stroke_start = index;
      }
    }
    analysis.point_count = trace.size() - stroke_start;
    if (analysis.point_count < 2) {
      return analysis;
    }

    // 图表只展示最近的接收间隔，分析也限制在相同窗口，避免长笔划时
    // 在 UI 线程中反复排序整个轨迹而干扰检测工具自身的响应。
    const auto analysis_start = analysis.point_count > max_graph_samples + 1 ?
                                  trace.size() - (max_graph_samples + 1) :
                                  stroke_start;
    const auto analyzed_point_count = trace.size() - analysis_start;

    static const auto performance_frequency = [] {
      LARGE_INTEGER frequency {};
      return QueryPerformanceFrequency(&frequency) ? static_cast<double>(frequency.QuadPart) : 0.0;
    }();
    std::vector<double> intervals;
    std::vector<double> turn_angles;
    intervals.reserve(analyzed_point_count - 1);
    turn_angles.reserve(analyzed_point_count > 2 ? analyzed_point_count - 2 : 0);

    bool have_previous_vector = false;
    double previous_dx {};
    double previous_dy {};
    for (std::size_t index = analysis_start + 1; index < trace.size(); ++index) {
      const auto &previous = trace[index - 1];
      const auto &current = trace[index];
      const auto interval = sampling_interval_ms(previous, current, performance_frequency);
      if (interval > 0.0 && interval <= 1000.0) {
        intervals.push_back(interval);
      }

      const auto dx = static_cast<double>(current.point.x - previous.point.x);
      const auto dy = static_cast<double>(current.point.y - previous.point.y);
      const auto length = std::hypot(dx, dy);
      if (length == 0.0) {
        continue;
      }

      if (have_previous_vector) {
        const auto previous_length = std::hypot(previous_dx, previous_dy);
        const auto cosine = std::clamp(
          (previous_dx * dx + previous_dy * dy) / (previous_length * length),
          -1.0,
          1.0
        );
        turn_angles.push_back(std::acos(cosine) * 180.0 / std::numbers::pi);
      }
      previous_dx = dx;
      previous_dy = dy;
      have_previous_vector = true;
    }

    if (!intervals.empty()) {
      analysis.interval_median_ms = percentile(intervals, 0.50);
      analysis.interval_p95_ms = percentile(intervals, 0.95);
      analysis.interval_max_ms = *std::max_element(intervals.begin(), intervals.end());
      analysis.recent_intervals_ms = intervals;
    }
    if (!turn_angles.empty()) {
      analysis.turn_median_degrees = percentile(turn_angles, 0.50);
      analysis.turn_p95_degrees = percentile(turn_angles, 0.95);
    }
    return analysis;
  }

  /** Clear captured and imported drawing state while preserving the current log. */
  void
  clear_capture_state(app_state_t &state) {
    state.pen_trace.clear();
    state.mouse_trace.clear();
    state.sampling_analysis = {};
    state.sampling_analysis_dirty = false;
    state.stats = {};
    state.pen_event_seen = false;
    state.mouse_event_seen = false;
    state.pen_in_contact = false;
    state.pen_trace_break_pending = false;
    state.mouse_in_contact = false;
    state.last_pen_point = {};
    state.last_mouse_point = {};
    state.last_mouse_point_available = false;
    state.last_pen_tick = {};
    state.last_pointer_id = {};
    state.last_pressure = {};
    state.last_pressure_available = false;
    state.last_tilt_x = {};
    state.last_tilt_y = {};
    state.last_tilt_available = false;
    state.last_rotation = {};
    state.last_rotation_available = false;
    state.pen_data.samples.clear();
    state.pen_data.truncated = false;
    state.capture_origin_performance_count = {};
    state.capture_origin_pointer_time = {};
    state.capture_time_origin_set = false;
    state.pen_read_error_reported = false;
    state.pen_history_error_reported = false;
    state.pen_history_limit_reported = false;
    state.pen_history_allocation_error_reported = false;
    state.viewing_imported_data = false;
    state.canvas_dirty = true;
    if (state.filter_promoted_mouse_checkbox != nullptr) {
      EnableWindow(state.filter_promoted_mouse_checkbox, TRUE);
    }
  }

  /** Convert protocol polar tilt into the Windows X/Y tilt representation. */
  std::pair<std::int32_t, std::int32_t>
  protocol_tilt_to_windows(std::uint32_t rotation, std::int32_t tilt) {
    const auto rotation_radians = static_cast<double>(rotation) * (std::numbers::pi / 180.0);
    const auto tilt_radians = static_cast<double>(tilt) * (std::numbers::pi / 180.0);
    const auto radial = std::sin(tilt_radians);
    const auto z = std::cos(tilt_radians);
    return {
      static_cast<std::int32_t>(std::atan2(std::sin(-rotation_radians) * radial, z) * 180.0 / std::numbers::pi),
      static_cast<std::int32_t>(std::atan2(std::cos(-rotation_radians) * radial, z) * 180.0 / std::numbers::pi),
    };
  }

  /** Replace the canvas with samples loaded from a stylus data file. */
  void
  apply_imported_data(app_state_t &state, const std::filesystem::path &path, const imported_pen_data_t &data) {
    clear_capture_state(state);
    state.pen_data = data;

    const auto canvas_width = std::max(1L, state.canvas.right - state.canvas.left - 2);
    const auto canvas_height = std::max(1L, state.canvas.bottom - state.canvas.top - 2);
    bool in_contact = false;
    bool started_with_down = false;
    std::size_t complete_strokes {};
    std::size_t partial_strokes {};
    std::size_t unterminated_strokes {};
    std::size_t contact_samples {};

    for (const auto &sample : data.samples) {
      const POINT point {
        state.canvas.left + 1 + static_cast<LONG>(std::lround(sample.x * canvas_width)),
        state.canvas.top + 1 + static_cast<LONG>(std::lround(sample.y * canvas_height)),
      };
      const bool contact_event = sample.event_type == stylus_event_down ||
                                 sample.event_type == stylus_event_move ||
                                 sample.event_type == stylus_event_button_only ||
                                 sample.event_type == stylus_event_up;
      const auto pressure = static_cast<UINT32>(std::lround(sample.pressure * pen_pressure_max));
      const bool pressure_available = contact_event && pressure != 0;
      if (contact_event) {
        ++contact_samples;
      }
      const auto pointer_time = static_cast<UINT32>(std::min<std::uint64_t>(
        sample.timestamp_us / 1000,
        std::numeric_limits<UINT32>::max()
      ));

      const auto append_sample = [&](bool break_before) {
        append_trace(state.pen_trace, {
          point,
          break_before,
          pressure,
          pressure_available,
          pointer_time,
          0,
        });
      };

      switch (sample.event_type) {
        case stylus_event_down:
          if (in_contact && started_with_down) {
            ++unterminated_strokes;
          }
          ++state.stats.pen_down;
          append_sample(true);
          in_contact = true;
          started_with_down = true;
          break;
        case stylus_event_move:
        case stylus_event_button_only:
          ++state.stats.pen_update;
          if (!in_contact) {
            ++partial_strokes;
            in_contact = true;
            started_with_down = false;
            append_sample(true);
          }
          else {
            append_sample(false);
          }
          break;
        case stylus_event_up:
          ++state.stats.pen_up;
          if (in_contact) {
            append_sample(false);
            if (started_with_down) {
              ++complete_strokes;
            }
          }
          in_contact = false;
          started_with_down = false;
          break;
        case stylus_event_cancel:
        case stylus_event_cancel_all:
          if (in_contact && started_with_down) {
            ++unterminated_strokes;
          }
          in_contact = false;
          started_with_down = false;
          break;
        case stylus_event_hover:
        case stylus_event_hover_leave:
          ++state.stats.pen_hover;
          break;
        default:
          break;
      }

      if (contact_event) {
        state.last_pen_point = point;
        state.last_pressure = pressure;
        state.last_pressure_available = pressure_available;
        const bool rotation_available = sample.rotation != stylus_rotation_unknown;
        const bool tilt_available = rotation_available && sample.tilt != stylus_tilt_unknown;
        state.last_rotation = rotation_available ? sample.rotation : 0;
        state.last_rotation_available = rotation_available;
        std::int32_t tilt_x {};
        std::int32_t tilt_y {};
        if (tilt_available) {
          const auto tilt = protocol_tilt_to_windows(sample.rotation, sample.tilt);
          tilt_x = tilt.first;
          tilt_y = tilt.second;
        }
        state.last_tilt_x = tilt_x;
        state.last_tilt_y = tilt_y;
        state.last_tilt_available = tilt_available;

        if (pressure_available) {
          state.stats.pressure_seen = true;
          state.stats.min_pressure = std::min(state.stats.min_pressure, pressure);
          state.stats.max_pressure = std::max(state.stats.max_pressure, pressure);
        }
        if (rotation_available) {
          state.stats.rotation_seen = true;
          state.stats.min_rotation = std::min(state.stats.min_rotation, sample.rotation);
          state.stats.max_rotation = std::max(state.stats.max_rotation, sample.rotation);
        }
        if (tilt_available) {
          state.stats.tilt_seen = true;
          state.stats.min_tilt_x = std::min(state.stats.min_tilt_x, tilt_x);
          state.stats.max_tilt_x = std::max(state.stats.max_tilt_x, tilt_x);
          state.stats.min_tilt_y = std::min(state.stats.min_tilt_y, tilt_y);
          state.stats.max_tilt_y = std::max(state.stats.max_tilt_y, tilt_y);
        }
      }
    }
    if (in_contact && started_with_down) {
      ++unterminated_strokes;
    }

    state.pen_event_seen = true;
    state.viewing_imported_data = true;
    if (state.filter_promoted_mouse_checkbox != nullptr) {
      EnableWindow(state.filter_promoted_mouse_checkbox, FALSE);
    }
    state.sampling_analysis = analyze_sampling(state.pen_trace);
    state.sampling_analysis_dirty = false;
    state.last_event = L"已导入 " + path.filename().wstring();

    std::wostringstream report;
    report << L"已导入 " << data.samples.size() << L" 个手写笔样本，完整笔划 "
           << complete_strokes << L" 段";
    if (data.truncated) {
      report << L"，源文件已在样本上限处截断";
    }
    if (partial_strokes != 0) {
      report << L"，缺少 DOWN 的部分笔划 " << partial_strokes << L" 段";
    }
    if (unterminated_strokes != 0) {
      report << L"，缺少结束事件的笔划 " << unterminated_strokes << L" 段";
    }
    if (contact_samples > state.pen_trace.size()) {
      report << L"。画布仅显示最近 " << state.pen_trace.size() << L" 个接触样本";
    }
    report << L"。采样图使用数据文件中的时间戳；点击“清空画布”可返回实时检测。";
    state.last_report = report.str();

    write_log_line(state,
      L"IMPORT | samples=" + std::to_wstring(data.samples.size()) +
        L", complete=" + std::to_wstring(complete_strokes) +
        L", missingDown=" + std::to_wstring(partial_strokes) +
        L", missingEnd=" + std::to_wstring(unterminated_strokes));
  }

  /** Select and import a stylus data file. */
  void
  import_stylus_data(HWND window, app_state_t &state) {
    std::array<wchar_t, 32768> file_path {};
    std::wstring directory_error;
    const auto directory_ready = ensure_recording_directory(state.recording_directory, directory_error);
    const auto initial_directory = state.recording_directory.wstring();

    OPENFILENAMEW dialog {
      .lStructSize = sizeof(OPENFILENAMEW),
      .hwndOwner = window,
      .lpstrFilter = L"手写笔数据 (*.dat)\0*.dat\0所有文件 (*.*)\0*.*\0\0",
      .lpstrFile = file_path.data(),
      .nMaxFile = static_cast<DWORD>(file_path.size()),
      .lpstrInitialDir = directory_ready ? initial_directory.c_str() : nullptr,
      .lpstrTitle = L"导入手写笔数据",
      .Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR,
      .lpstrDefExt = L"dat",
    };
    if (!GetOpenFileNameW(&dialog)) {
      if (const auto dialog_error = CommDlgExtendedError(); dialog_error != 0) {
        MessageBoxW(
          window,
          (L"无法打开文件选择窗口，错误码：" + std::to_wstring(dialog_error)).c_str(),
          window_title,
          MB_OK | MB_ICONERROR
        );
      }
      return;
    }

    imported_pen_data_t data;
    std::wstring error;
    const std::filesystem::path path(file_path.data());
    if (!read_stylus_data(path, data, error)) {
      MessageBoxW(window, error.c_str(), window_title, MB_OK | MB_ICONERROR);
      return;
    }

    apply_imported_data(state, path, data);
    InvalidateRect(window, nullptr, FALSE);
  }

  /** Build a timestamped default name for a recorded data file. */
  std::wstring
  make_data_file_name() {
    SYSTEMTIME time {};
    GetLocalTime(&time);
    std::wostringstream default_name;
    default_name << L"stylus-input-"
                 << std::setfill(L'0')
                 << std::setw(4) << time.wYear
                 << std::setw(2) << time.wMonth
                 << std::setw(2) << time.wDay << L'-'
                 << std::setw(2) << time.wHour
                 << std::setw(2) << time.wMinute
                 << std::setw(2) << time.wSecond << L'-'
                 << std::setw(3) << time.wMilliseconds << L'-'
                 << GetCurrentProcessId() << L".dat";
    return default_name.str();
  }

  /** Ask the user where a data file should be saved. */
  bool
  select_data_save_path(HWND window, const wchar_t *title, const std::filesystem::path &initial_directory, std::filesystem::path &path) {
    std::array<wchar_t, 32768> file_path {};
    const auto file_name = make_data_file_name();
    file_name.copy(file_path.data(), std::min(file_name.size(), file_path.size() - 1));

    OPENFILENAMEW dialog {
      .lStructSize = sizeof(OPENFILENAMEW),
      .hwndOwner = window,
      .lpstrFilter = L"手写笔数据 (*.dat)\0*.dat\0所有文件 (*.*)\0*.*\0\0",
      .lpstrFile = file_path.data(),
      .nMaxFile = static_cast<DWORD>(file_path.size()),
      .lpstrInitialDir = initial_directory.empty() ? nullptr : initial_directory.c_str(),
      .lpstrTitle = title,
      .Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR,
      .lpstrDefExt = L"dat",
    };
    if (!GetSaveFileNameW(&dialog)) {
      if (const auto dialog_error = CommDlgExtendedError(); dialog_error != 0) {
        MessageBoxW(
          window,
          (L"无法打开文件保存窗口，错误码：" + std::to_wstring(dialog_error)).c_str(),
          window_title,
          MB_OK | MB_ICONERROR
        );
      }
      return false;
    }
    path = file_path.data();
    return true;
  }

  /** Select a destination and export the current stylus data. */
  void
  export_stylus_data(HWND window, app_state_t &state) {
    if (state.pen_data.samples.empty()) {
      MessageBoxW(window, L"当前没有可导出的手写笔数据。", window_title, MB_OK | MB_ICONINFORMATION);
      return;
    }

    std::filesystem::path path;
    std::wstring directory_error;
    const auto directory_ready = ensure_recording_directory(state.recording_directory, directory_error);
    if (!select_data_save_path(window, L"导出手写笔数据", directory_ready ? state.recording_directory : std::filesystem::path {}, path)) {
      return;
    }
    std::wstring error;
    if (!write_stylus_data(path, state.pen_data, error)) {
      MessageBoxW(window, error.c_str(), window_title, MB_OK | MB_ICONERROR);
      return;
    }

    state.last_event = L"已导出 " + path.filename().wstring();
    state.last_report = L"已导出 " + std::to_wstring(state.pen_data.samples.size()) + L" 个手写笔样本。";
    if (state.pen_data.truncated) {
      state.last_report += L"本次记录达到样本上限，文件只包含达到上限前的数据。";
    }
    write_log_line(state, L"EXPORT | samples=" + std::to_wstring(state.pen_data.samples.size()));
    state.canvas_dirty = true;
    InvalidateRect(window, nullptr, FALSE);
  }

  /** Keep controls consistent while a recording owns the current data set. */
  void
  update_recording_controls(app_state_t &state) {
    if (state.record_data_button != nullptr) {
      SetWindowTextW(state.record_data_button, state.recording ? L"停止录制" : L"开始录制");
    }
    const auto enabled = state.recording ? FALSE : TRUE;
    if (state.clear_button != nullptr) {
      EnableWindow(state.clear_button, enabled);
    }
    if (state.import_data_button != nullptr) {
      EnableWindow(state.import_data_button, enabled);
    }
    if (state.export_data_button != nullptr) {
      EnableWindow(state.export_data_button, enabled);
    }
    if (state.filter_promoted_mouse_checkbox != nullptr) {
      EnableWindow(state.filter_promoted_mouse_checkbox, enabled);
    }
  }

  /** Begin a measurement recording and persist its file header immediately. */
  void
  start_recording(HWND window, app_state_t &state) {
    std::wstring error;
    if (!ensure_recording_directory(state.recording_directory, error)) {
      MessageBoxW(window, error.c_str(), window_title, MB_OK | MB_ICONERROR);
      return;
    }
    const auto path = state.recording_directory / make_data_file_name();

    clear_capture_state(state);
    try {
      // 录制开始时一次性申请上限容量，避免绘制过程中扩容造成停顿或异常。
      state.pen_data.samples.reserve(max_data_samples);
    }
    catch (const std::exception &) {
      MessageBoxW(window, L"无法为录制数据分配内存。", window_title, MB_OK | MB_ICONERROR);
      return;
    }
    catch (...) {
      MessageBoxW(window, L"准备录制数据时发生未知异常。", window_title, MB_OK | MB_ICONERROR);
      return;
    }
    if (!open_recording_file(state, path, error)) {
      MessageBoxW(window, error.c_str(), window_title, MB_OK | MB_ICONERROR);
      return;
    }

    state.recording_path = path;
    state.recording = true;
    state.last_event = L"录制已开始";
    state.last_report = L"正在录制画布内的手写笔输入。文件保存在专用录制目录：" + state.recording_directory.wstring();
    update_recording_controls(state);
    write_log_line(state, L"RECORD | started");
    state.canvas_dirty = true;
    InvalidateRect(window, nullptr, FALSE);
  }

  /** Finish the active recording and persist its final checkpoint. */
  bool
  stop_recording(HWND window, app_state_t &state, bool closing = false) {
    if (!state.recording) {
      return true;
    }

    std::wstring error;
    const auto saved = close_recording_file(state, error);
    state.recording = false;
    update_recording_controls(state);
    if (!saved) {
      state.last_event = L"录制保存失败";
      state.last_report = closing ?
                            error + L" 窗口仍将关闭。" :
                            error + L" 当前数据仍保留在内存中，可使用“导出数据”另存。";
      write_log_line(state, L"RECORD | save failed");
      MessageBoxW(window, state.last_report.c_str(), window_title, MB_OK | MB_ICONERROR);
      state.canvas_dirty = true;
      InvalidateRect(window, nullptr, FALSE);
      return false;
    }

    state.last_event = L"录制已保存为 " + state.recording_path.filename().wstring();
    state.last_report = L"录制完成，共保存 " + std::to_wstring(state.pen_data.samples.size()) + L" 个手写笔样本。";
    if (state.pen_data.truncated) {
      state.last_report += L"本次录制达到样本上限，文件只包含达到上限前的数据。";
    }
    write_log_line(state, L"RECORD | stopped | samples=" + std::to_wstring(state.pen_data.samples.size()));
    state.log_writer.flush();
    state.canvas_dirty = true;
    InvalidateRect(window, nullptr, FALSE);
    return true;
  }

  /** Stop recording after an incremental checkpoint failure while retaining samples in memory. */
  void
  handle_recording_checkpoint_failure(HWND window, app_state_t &state, const std::wstring &error) {
    state.recording_stream.close();
    state.recording = false;
    state.recording_checkpoint_requested = false;
    update_recording_controls(state);
    state.last_event = L"录制保存失败";
    state.last_report = error + L" 当前数据仍保留在内存中，可使用“导出数据”另存。";
    write_log_line(state, L"RECORD | checkpoint failed");
    MessageBoxW(window, state.last_report.c_str(), window_title, MB_OK | MB_ICONERROR);
    state.canvas_dirty = true;
    InvalidateRect(window, nullptr, FALSE);
  }

  /** Toggle the explicit recording state. */
  void
  toggle_recording(HWND window, app_state_t &state) {
    if (state.recording) {
      stop_recording(window, state);
    }
    else {
      start_recording(window, state);
    }
  }

  /** Test whether a client coordinate is inside the drawing canvas. */
  bool
  point_in_canvas(const app_state_t &state, POINT point) {
    return PtInRect(&state.canvas, point) != FALSE;
  }

  /** Identify a mouse message promoted from a pen pointer. */
  bool
  is_pen_promoted_mouse(std::uintptr_t raw_extra_info) {
    const auto extra_info = static_cast<std::uint32_t>(
      raw_extra_info
    );
    return (extra_info & mouse_pointer_signature_mask) == mouse_pointer_signature &&
           (extra_info & mouse_pointer_touch_flag) == 0;
  }

  /** Correlate a mouse message with the latest pen message. */
  bool
  is_correlated_mouse(const app_state_t &state, POINT point) {
    if (state.last_pen_tick == 0 || GetTickCount64() - state.last_pen_tick > 100) {
      return false;
    }

    return std::abs(point.x - state.last_pen_point.x) <= 6 &&
           std::abs(point.y - state.last_pen_point.y) <= 6;
  }

  /** Format the current local time for a report. */
  std::wstring
  current_time_text() {
    SYSTEMTIME time {};
    GetLocalTime(&time);

    std::wostringstream output;
    output << std::setfill(L'0')
           << std::setw(4) << time.wYear << L'-'
           << std::setw(2) << time.wMonth << L'-'
           << std::setw(2) << time.wDay << L' '
           << std::setw(2) << time.wHour << L':'
           << std::setw(2) << time.wMinute << L':'
           << std::setw(2) << time.wSecond << L'.'
           << std::setw(3) << time.wMilliseconds;
    return output.str();
  }

  /** Build the latest diagnostic report. */
  std::wstring
  make_report(const app_state_t &state) {
    const auto &stats = state.stats;
    const auto pen_events = stats.pen_down + stats.pen_update + stats.pen_up + stats.pen_hover;
    const auto mouse_events = stats.mouse_down + stats.mouse_move + stats.mouse_up;

    std::wstring conclusion;
    if (pen_events == 0 && mouse_events == 0) {
      conclusion = L"等待输入：本周期没有检测到画布内的输入变化。";
    }
    else if (pen_events == 0) {
      conclusion = L"未识别为手写笔：只收到了鼠标消息，没有收到 PT_PEN。";
    }
    else if (!stats.pressure_seen) {
      conclusion = L"已收到 PT_PEN，但本周期没有有效压力字段。";
    }
    else if (state.filter_promoted_mouse && stats.filtered_promoted_mouse != 0) {
      conclusion = L"已过滤画布内的笔兼容鼠标消息；可与关闭开关时的蓝色笔迹进行 A/B 对比。";
    }
    else if (stats.promoted_mouse != 0 || stats.correlated_mouse != 0) {
      conclusion = L"手写笔和压力正常；同时存在与笔关联的兼容鼠标消息，绘图程序应避免混用两条轨迹。";
    }
    else {
      conclusion = L"手写笔输入正常：已收到 PT_PEN、压力和连续轨迹。";
    }

    std::wostringstream output;
    output << current_time_text()
           << L" | PT_PEN: down=" << stats.pen_down
           << L", move=" << stats.pen_update
           << L", up=" << stats.pen_up
           << L", hover=" << stats.pen_hover
           << L", errors=" << stats.pen_errors;

    if (stats.pressure_seen) {
      output << L" | pressure=" << stats.min_pressure << L".." << stats.max_pressure;
    }
    else {
      output << L" | pressure=none";
    }

    if (stats.tilt_seen) {
      output << L" | tiltX=" << stats.min_tilt_x << L".." << stats.max_tilt_x
             << L", tiltY=" << stats.min_tilt_y << L".." << stats.max_tilt_y;
    }
    else {
      output << L" | tilt=none";
    }

    if (stats.rotation_seen) {
      output << L" | rotation=" << stats.min_rotation << L".." << stats.max_rotation;
    }
    else {
      output << L" | rotation=none";
    }

    const auto &sampling = state.sampling_analysis;
    if (!sampling.recent_intervals_ms.empty()) {
      output << L" | host PT_PEN interval: strokePoints=" << sampling.point_count
             << L", recentIntervals=" << sampling.recent_intervals_ms.size()
             << L", median=" << std::fixed << std::setprecision(1) << sampling.interval_median_ms
             << L"ms, p95=" << sampling.interval_p95_ms
             << L"ms, max=" << sampling.interval_max_ms
             << L"ms, turnMedian=" << sampling.turn_median_degrees
             << L"deg, turnP95=" << sampling.turn_p95_degrees << L"deg";
    }
    else {
      output << L" | host PT_PEN interval=waiting";
    }

    output << L" | mouse: down=" << stats.mouse_down
           << L", move=" << stats.mouse_move
           << L", up=" << stats.mouse_up
           << L", promoted=" << stats.promoted_mouse
           << L", correlated=" << stats.correlated_mouse
           << L", filtered=" << stats.filtered_promoted_mouse
           << L" | " << conclusion;
    return output.str();
  }

  /** Return a stable name for a pointer message. */
  std::wstring_view
  pen_event_name(UINT message, bool in_contact) {
    switch (message) {
      case WM_POINTERDOWN:
        return L"PEN_DOWN";
      case WM_POINTERUP:
        return L"PEN_UP";
      case WM_POINTERUPDATE:
        return in_contact ? L"PEN_UPDATE" : L"PEN_HOVER";
      case WM_POINTERENTER:
        return L"PEN_ENTER";
      case WM_POINTERLEAVE:
        return L"PEN_LEAVE";
      case WM_POINTERCAPTURECHANGED:
        return L"PEN_CAPTURE_CHANGED";
      default:
        return L"PEN_UNKNOWN";
    }
  }

  /** Return a stable name for a mouse message. */
  std::wstring_view
  mouse_event_name(UINT message) {
    switch (message) {
      case WM_LBUTTONDOWN:
        return L"MOUSE_DOWN";
      case WM_LBUTTONUP:
        return L"MOUSE_UP";
      case WM_MOUSEMOVE:
        return L"MOUSE_MOVE";
      default:
        return L"MOUSE_UNKNOWN";
    }
  }

  /** Write one UTF-8 line to the current diagnostic log. */
  void
  write_log_line(app_state_t &state, std::wstring_view line) noexcept {
    try {
      state.log_writer.write(to_utf8(line));
    }
    catch (...) {
      // 状态日志不是检测功能的必要条件，编码失败时直接停用本次写入。
    }
  }

  /** Open a fresh log for the current probe run. */
  void
  open_log(app_state_t &state) {
    std::wstring error;
    if (!ensure_recording_directory(state.recording_directory, error)) {
      state.last_report = error;
      return;
    }
    if (!state.log_writer.open(state.log_path)) {
      state.last_report = L"无法创建运行日志。";
      return;
    }

    write_log_line(state, L"START | Sunshine stylus input probe");
    state.log_writer.flush();
  }

  /** Update the latest visible pen event without logging every sample. */
  void
  update_pen_status(app_state_t &state, UINT message, const POINTER_PEN_INFO &pen_info, POINT client_point) {
    const auto in_contact = (pen_info.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
    state.pen_event_seen = true;
    state.last_event = std::wstring(pen_event_name(message, in_contact)) +
                       L" client=(" + std::to_wstring(client_point.x) + L"," +
                       std::to_wstring(client_point.y) + L")";
  }

  /** Update the latest visible mouse event without logging every message. */
  void
  update_mouse_status(app_state_t &state, UINT message, POINT point) {
    state.mouse_event_seen = true;
    state.last_event = std::wstring(mouse_event_name(message)) +
                       L" client=(" + std::to_wstring(point.x) + L"," +
                       std::to_wstring(point.y) + L")";
  }

  /** Copy text to the Windows clipboard. */
  bool
  copy_text_to_clipboard(HWND window, const std::wstring &text) {
    if (!OpenClipboard(window)) {
      return false;
    }

    EmptyClipboard();
    bool copied = false;
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory != nullptr) {
      if (auto destination = GlobalLock(memory); destination != nullptr) {
        std::memcpy(destination, text.c_str(), bytes);
        GlobalUnlock(memory);
        if (SetClipboardData(CF_UNICODETEXT, memory) != nullptr) {
          memory = nullptr;
          copied = true;
        }
      }
      if (memory != nullptr) {
        GlobalFree(memory);
      }
    }
    CloseClipboard();
    return copied;
  }

  /** Recalculate the canvas and button positions. */
  void
  update_layout(HWND window, app_state_t &state, bool remap_points = false) {
    RECT client {};
    GetClientRect(window, &client);
    const auto previous_canvas = state.canvas;

    const auto margin = static_cast<LONG>(scale_for_dpi(16, state.dpi));
    const auto button_width = static_cast<LONG>(scale_for_dpi(96, state.dpi));
    const auto button_height = static_cast<LONG>(scale_for_dpi(30, state.dpi));
    const auto button_gap = static_cast<LONG>(scale_for_dpi(8, state.dpi));
    const auto first_button_x = std::max(margin, client.right - margin - button_width * 5 - button_gap * 4);

    const auto button_y = scale_for_dpi(64, state.dpi);
    MoveWindow(state.clear_button, first_button_x, button_y, button_width, button_height, TRUE);
    MoveWindow(state.copy_button, first_button_x + button_width + button_gap, button_y, button_width, button_height, TRUE);
    MoveWindow(state.import_data_button, first_button_x + (button_width + button_gap) * 2, button_y, button_width, button_height, TRUE);
    MoveWindow(state.export_data_button, first_button_x + (button_width + button_gap) * 3, button_y, button_width, button_height, TRUE);
    MoveWindow(state.open_log_button, first_button_x + (button_width + button_gap) * 4, button_y, button_width, button_height, TRUE);
    MoveWindow(state.filter_promoted_mouse_checkbox, margin, button_y, scale_for_dpi(270, state.dpi), button_height, TRUE);
    MoveWindow(state.record_data_button, margin + scale_for_dpi(278, state.dpi), button_y, button_width, button_height, TRUE);

    const auto footer_button_width = static_cast<LONG>(scale_for_dpi(112, state.dpi));
    const auto footer_button_y = client.bottom - margin - button_height;
    const auto open_recording_folder_x = client.right - margin - footer_button_width;
    const auto copy_log_path_x = open_recording_folder_x - button_gap - footer_button_width;
    MoveWindow(state.copy_log_path_button, copy_log_path_x, footer_button_y, footer_button_width, button_height, TRUE);
    MoveWindow(state.open_recording_folder_button, open_recording_folder_x, footer_button_y, footer_button_width, button_height, TRUE);

    const auto canvas_top = static_cast<LONG>(scale_for_dpi(130, state.dpi));
    const auto footer_height = static_cast<LONG>(scale_for_dpi(300, state.dpi));

    state.canvas = {
      margin,
      canvas_top,
      std::max(margin + 1, client.right - margin),
      std::max(canvas_top + 1, client.bottom - footer_height),
    };

    const auto previous_width = previous_canvas.right - previous_canvas.left;
    const auto previous_height = previous_canvas.bottom - previous_canvas.top;
    const auto current_width = state.canvas.right - state.canvas.left;
    const auto current_height = state.canvas.bottom - state.canvas.top;
    if (!remap_points || previous_width <= 1 || previous_height <= 1 ||
        (previous_width == current_width && previous_height == current_height)) {
      return;
    }

    const auto remap_point = [&](POINT &point) {
      point.x = state.canvas.left + static_cast<LONG>(std::lround(
                                      static_cast<double>(point.x - previous_canvas.left) * current_width /
                                      previous_width
      ));
      point.y = state.canvas.top + static_cast<LONG>(std::lround(
                                     static_cast<double>(point.y - previous_canvas.top) * current_height /
                                     previous_height
      ));
    };
    for (auto &point : state.pen_trace) {
      remap_point(point.point);
    }
    for (auto &point : state.mouse_trace) {
      remap_point(point.point);
    }
    if (state.pen_event_seen) {
      remap_point(state.last_pen_point);
    }
  }

  /** Draw one recorded input trace. */
  void
  draw_trace(HDC device_context, const std::deque<trace_point_t> &trace, COLORREF color, int width, int style) {
    if (trace.size() < 2) {
      return;
    }

    auto pen = CreatePen(style, width, color);
    auto old_pen = SelectObject(device_context, pen);
    POINT previous {};
    bool have_previous = false;

    for (const auto &point : trace) {
      if (point.break_before || !have_previous) {
        previous = point.point;
        have_previous = true;
        continue;
      }

      MoveToEx(device_context, previous.x, previous.y, nullptr);
      LineTo(device_context, point.point.x, point.point.y);
      previous = point.point;
    }

    SelectObject(device_context, old_pen);
    DeleteObject(pen);
  }

  /** Draw the pen trace with a line width determined by the reported pressure. */
  void
  draw_pen_trace(HDC device_context, const std::deque<trace_point_t> &trace, UINT dpi) {
    if (trace.size() < 2) {
      return;
    }

    constexpr std::size_t pressure_levels = 8;
    const auto minimum_width = std::max(1, scale_for_dpi(2, dpi));
    const auto maximum_width = std::max(minimum_width, scale_for_dpi(12, dpi));
    std::array<HPEN, pressure_levels> pens {};
    for (std::size_t level = 0; level < pens.size(); ++level) {
      const auto width = minimum_width + MulDiv(
                                           maximum_width - minimum_width,
                                           static_cast<int>(level),
                                           static_cast<int>(pressure_levels - 1)
                                         );
      pens[level] = CreatePen(PS_SOLID, width, RGB(30, 105, 220));
    }

    POINT previous {};
    bool have_previous = false;
    std::size_t active_level {};
    if (pens[active_level] == nullptr) {
      for (const auto pen : pens) {
        if (pen != nullptr) {
          DeleteObject(pen);
        }
      }
      return;
    }
    auto old_pen = SelectObject(device_context, pens[active_level]);
    for (const auto &point : trace) {
      if (point.break_before || !have_previous) {
        previous = point.point;
        have_previous = true;
        continue;
      }

      const auto normalized_pressure = point.pressure_available ?
                                           std::clamp(
                                             static_cast<float>(point.pressure) / static_cast<float>(pen_pressure_max),
                                             0.0f,
                                             1.0f
      ) :
                                           0.5f;
      const auto level = static_cast<std::size_t>(std::lround(normalized_pressure * static_cast<float>(pressure_levels - 1)));
      if (level != active_level && pens[level] != nullptr) {
        SelectObject(device_context, pens[level]);
        active_level = level;
      }
      MoveToEx(device_context, previous.x, previous.y, nullptr);
      LineTo(device_context, point.point.x, point.point.y);
      previous = point.point;
    }

    SelectObject(device_context, old_pen);
    for (const auto pen : pens) {
      if (pen != nullptr) {
        DeleteObject(pen);
      }
    }
  }

  /** Draw recent delivery intervals for the most recent stroke. */
  void
  draw_sampling_graph(HDC device_context, RECT bounds, const sampling_analysis_t &analysis, UINT dpi, bool imported_data) {
    auto background = CreateSolidBrush(RGB(250, 252, 255));
    FillRect(device_context, &bounds, background);
    DeleteObject(background);
    FrameRect(device_context, &bounds, static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH)));

    const auto inset = scale_for_dpi(6, dpi);
    RECT title_rect {bounds.left + inset, bounds.top + inset, bounds.right - inset, bounds.top + scale_for_dpi(22, dpi)};
    const auto interval_label = imported_data ? L"导入数据样本间隔" : L"主机 PT_PEN 接收间隔";
    if (analysis.recent_intervals_ms.empty()) {
      const auto waiting_text = std::wstring(interval_label) + L"：等待至少两个连续的落笔点";
      DrawTextW(device_context, waiting_text.c_str(), -1, &title_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
      return;
    }

    std::wostringstream title;
    title << interval_label << L"：最近 " << analysis.recent_intervals_ms.size() << L" 个间隔"
          << L"，中位 " << std::fixed << std::setprecision(1) << analysis.interval_median_ms
          << L" ms，P95 " << analysis.interval_p95_ms
          << L" ms，最大 " << analysis.interval_max_ms << L" ms"
          << L"；方向变化中位 " << analysis.turn_median_degrees
          << L"°，P95 " << analysis.turn_p95_degrees << L"°";
    const auto title_text = title.str();
    DrawTextW(device_context, title_text.c_str(), -1, &title_rect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

    RECT plot {
      bounds.left + scale_for_dpi(38, dpi),
      bounds.top + scale_for_dpi(25, dpi),
      bounds.right - inset,
      bounds.bottom - scale_for_dpi(7, dpi),
    };
    const auto graph_max = std::max(8.0, std::ceil(analysis.interval_max_ms * 1.15));
    auto axis_pen = CreatePen(PS_SOLID, 1, RGB(195, 202, 212));
    auto old_pen = SelectObject(device_context, axis_pen);
    MoveToEx(device_context, plot.left, plot.bottom, nullptr);
    LineTo(device_context, plot.right, plot.bottom);
    MoveToEx(device_context, plot.left, plot.top, nullptr);
    LineTo(device_context, plot.left, plot.bottom);

    const auto threshold_y = plot.bottom - static_cast<LONG>(std::lround((8.0 / graph_max) * (plot.bottom - plot.top)));
    auto threshold_pen = CreatePen(PS_DOT, 1, RGB(230, 170, 70));
    SelectObject(device_context, threshold_pen);
    MoveToEx(device_context, plot.left, threshold_y, nullptr);
    LineTo(device_context, plot.right, threshold_y);

    std::wostringstream max_label;
    max_label << std::fixed << std::setprecision(0) << graph_max << L" ms";
    const auto max_label_text = max_label.str();
    RECT max_label_rect {bounds.left + inset, plot.top - scale_for_dpi(3, dpi), plot.left - inset, plot.top + scale_for_dpi(16, dpi)};
    DrawTextW(device_context, max_label_text.c_str(), -1, &max_label_rect, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    RECT threshold_label_rect {bounds.left + inset, threshold_y - scale_for_dpi(8, dpi), plot.left - inset, threshold_y + scale_for_dpi(8, dpi)};
    DrawTextW(device_context, L"8 ms", -1, &threshold_label_rect, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    auto sample_pen = CreatePen(PS_SOLID, std::max(1, scale_for_dpi(2, dpi)), RGB(40, 142, 90));
    SelectObject(device_context, sample_pen);
    for (std::size_t index = 0; index < analysis.recent_intervals_ms.size(); ++index) {
      const auto x = analysis.recent_intervals_ms.size() == 1 ?
                       plot.left :
                       plot.left + MulDiv(
                                     plot.right - plot.left,
                                     static_cast<int>(index),
                                     static_cast<int>(analysis.recent_intervals_ms.size() - 1)
                                   );
      const auto normalized = std::clamp(analysis.recent_intervals_ms[index] / graph_max, 0.0, 1.0);
      const auto y = plot.bottom - static_cast<LONG>(std::lround(normalized * (plot.bottom - plot.top)));
      if (index == 0) {
        MoveToEx(device_context, x, y, nullptr);
      }
      else {
        LineTo(device_context, x, y);
      }
    }

    SelectObject(device_context, old_pen);
    DeleteObject(sample_pen);
    DeleteObject(threshold_pen);
    DeleteObject(axis_pen);
  }

  /** Paint the diagnostic canvas and latest state. */
  void
  paint_window(HWND window, app_state_t &state) {
    if (state.sampling_analysis_dirty) {
      state.sampling_analysis = analyze_sampling(state.pen_trace);
      state.sampling_analysis_dirty = false;
    }
    PAINTSTRUCT paint {};
    auto target_context = BeginPaint(window, &paint);

    RECT client {};
    GetClientRect(window, &client);
    const auto width = std::max(1L, client.right - client.left);
    const auto height = std::max(1L, client.bottom - client.top);

    auto memory_context = CreateCompatibleDC(target_context);
    auto bitmap = CreateCompatibleBitmap(target_context, width, height);
    auto old_bitmap = SelectObject(memory_context, bitmap);
    auto background = CreateSolidBrush(RGB(245, 247, 250));
    FillRect(memory_context, &client, background);
    DeleteObject(background);

    SetBkMode(memory_context, TRANSPARENT);
    SetTextColor(memory_context, RGB(30, 36, 45));
    auto font = state.ui_font != nullptr ? state.ui_font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto old_font = SelectObject(memory_context, font);

    const auto margin = scale_for_dpi(16, state.dpi);
    RECT title_rect {margin, scale_for_dpi(10, state.dpi), client.right - margin, scale_for_dpi(34, state.dpi)};
    DrawTextW(memory_context, L"Sunshine 手写笔输入检测", -1, &title_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT instruction_rect {margin, scale_for_dpi(36, state.dpi), client.right - margin, scale_for_dpi(60, state.dpi)};
    const auto instruction = state.recording ?
                               L"正在录制画布内的手写笔数据；完成后点击“停止录制”。蓝线线宽表示压感。" :
                             state.viewing_imported_data ?
                               L"正在查看导入数据；蓝线线宽表示压感。点击“清空画布”返回实时检测。" :
                               L"请在白色检测区内按下后拖动。仅画布内接触计入统计；蓝线=PT_PEN，红线=未过滤的鼠标消息。";
    DrawTextW(memory_context, instruction, -1, &instruction_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    auto canvas_brush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(memory_context, &state.canvas, canvas_brush);
    DeleteObject(canvas_brush);
    FrameRect(memory_context, &state.canvas, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

    SaveDC(memory_context);
    IntersectClipRect(memory_context, state.canvas.left + 1, state.canvas.top + 1, state.canvas.right - 1, state.canvas.bottom - 1);
    draw_trace(memory_context, state.mouse_trace, RGB(220, 55, 65), std::max(1, scale_for_dpi(2, state.dpi)), PS_SOLID);
    draw_pen_trace(memory_context, state.pen_trace, state.dpi);
    RestoreDC(memory_context, -1);

    RECT sampling_graph_rect {
      margin,
      state.canvas.bottom + scale_for_dpi(8, state.dpi),
      client.right - margin,
      state.canvas.bottom + scale_for_dpi(90, state.dpi),
    };
    draw_sampling_graph(memory_context, sampling_graph_rect, state.sampling_analysis, state.dpi, state.viewing_imported_data);

    std::wostringstream live_text;
    live_text << L"当前：pointerId=" << state.last_pointer_id
              << L"，pressure=" << state.last_pressure
              << L"，tilt=(" << state.last_tilt_x << L", " << state.last_tilt_y << L")";
    RECT live_rect {margin, state.canvas.bottom + scale_for_dpi(96, state.dpi), client.right - margin, state.canvas.bottom + scale_for_dpi(120, state.dpi)};
    DrawTextW(memory_context, live_text.str().c_str(), -1, &live_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    std::wostringstream detail_text;
    detail_text << L"Pressure=";
    if (state.last_pressure_available) {
      const auto percentage = static_cast<double>(state.last_pressure) * 100.0 / pen_pressure_max;
      detail_text << state.last_pressure << L"/" << pen_pressure_max << L" (" << std::fixed << std::setprecision(1) << percentage << L"%)";
    }
    else {
      detail_text << L"not-reported";
    }
    detail_text << L" | Tilt=";
    if (state.last_tilt_available) {
      detail_text << L"(" << state.last_tilt_x << L", " << state.last_tilt_y << L")";
    }
    else {
      detail_text << L"not-reported";
    }
    detail_text << L" | Rotation=";
    if (state.last_rotation_available) {
      detail_text << state.last_rotation;
    }
    else {
      detail_text << L"not-reported";
    }
    detail_text << L" | 蓝色笔迹线宽=压感";
    RECT detail_rect {margin, state.canvas.bottom + scale_for_dpi(120, state.dpi), client.right - margin, state.canvas.bottom + scale_for_dpi(144, state.dpi)};
    DrawTextW(memory_context, detail_text.str().c_str(), -1, &detail_rect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

    std::wostringstream status_text;
    status_text << L"状态：" << (state.recording ? L"录制中" : state.viewing_imported_data ? L"导入数据" : L"实时检测")
                << L"，PT_PEN=" << (state.pen_event_seen ? L"已收到" : L"未收到")
                << L"，鼠标=" << (state.mouse_event_seen ? L"已收到" : L"未收到")
                << L"；本周期笔事件=" << (state.stats.pen_down + state.stats.pen_update + state.stats.pen_up + state.stats.pen_hover)
                << L"，鼠标事件=" << (state.stats.mouse_down + state.stats.mouse_move + state.stats.mouse_up)
                << L"，兼容鼠标=" << state.stats.promoted_mouse
                << L"，过滤=" << (state.filter_promoted_mouse ? L"开启" : L"关闭")
                << L"，已过滤=" << state.stats.filtered_promoted_mouse;
    if (state.recording) {
      status_text << L"，已录制样本=" << state.pen_data.samples.size();
    }
    RECT status_rect {margin, scale_for_dpi(98, state.dpi), client.right - margin, scale_for_dpi(122, state.dpi)};
    DrawTextW(memory_context, status_text.str().c_str(), -1, &status_rect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

    RECT event_rect {margin, state.canvas.bottom + scale_for_dpi(148, state.dpi), client.right - margin, state.canvas.bottom + scale_for_dpi(172, state.dpi)};
    DrawTextW(memory_context, (L"最近事件：" + state.last_event).c_str(), -1, &event_rect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

    RECT report_rect {margin, state.canvas.bottom + scale_for_dpi(176, state.dpi), client.right - margin, client.bottom - scale_for_dpi(40, state.dpi)};
    DrawTextW(memory_context, state.last_report.c_str(), -1, &report_rect, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);

    const auto log_text = L"运行日志：" + state.log_path.wstring();
    SetTextColor(memory_context, RGB(85, 92, 102));
    const auto footer_buttons_width = scale_for_dpi(112 * 2 + 8, state.dpi);
    RECT log_rect {margin, client.bottom - scale_for_dpi(40, state.dpi), client.right - margin - footer_buttons_width, client.bottom - scale_for_dpi(10, state.dpi)};
    DrawTextW(memory_context, log_text.c_str(), -1, &log_rect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);

    SelectObject(memory_context, old_font);
    BitBlt(target_context, 0, 0, width, height, memory_context, 0, 0, SRCCOPY);
    SelectObject(memory_context, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory_context);
    EndPaint(window, &paint);
  }

  /** Record one native pen sample for later export. */
  void
  record_pen_data_sample(app_state_t &state, UINT message, const POINTER_PEN_INFO &pen_info, POINT point) {
    if (!state.recording) {
      return;
    }

    std::uint8_t event_type {};
    switch (message) {
      case WM_POINTERDOWN:
        event_type = stylus_event_down;
        break;
      case WM_POINTERUP:
        event_type = stylus_event_up;
        break;
      case WM_POINTERUPDATE:
        event_type = (pen_info.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) != 0 ?
                       stylus_event_move :
                       stylus_event_hover;
        break;
      case WM_POINTERLEAVE:
        event_type = stylus_event_hover_leave;
        break;
      default:
        return;
    }

    if (state.pen_data.samples.size() >= max_data_samples) {
      if (!state.pen_data.truncated) {
        state.pen_data.truncated = true;
        state.recording_checkpoint_requested = true;
        write_log_line(state, L"DATA | sample limit reached");
      }
      return;
    }

    if (!state.capture_time_origin_set) {
      state.capture_origin_performance_count = pen_info.pointerInfo.PerformanceCount;
      state.capture_origin_pointer_time = pen_info.pointerInfo.dwTime;
      state.capture_time_origin_set = true;
    }

    std::uint64_t timestamp_us {};
    static const auto performance_frequency = [] {
      LARGE_INTEGER frequency {};
      return QueryPerformanceFrequency(&frequency) ? static_cast<std::uint64_t>(frequency.QuadPart) : 0;
    }();
    if (performance_frequency != 0 && state.capture_origin_performance_count != 0 &&
        pen_info.pointerInfo.PerformanceCount >= state.capture_origin_performance_count) {
      const auto elapsed = pen_info.pointerInfo.PerformanceCount - state.capture_origin_performance_count;
      timestamp_us = static_cast<std::uint64_t>(
        static_cast<long double>(elapsed) * 1000000.0L / performance_frequency
      );
    }
    else {
      timestamp_us = static_cast<std::uint32_t>(pen_info.pointerInfo.dwTime - state.capture_origin_pointer_time) * 1000ULL;
    }
    if (!state.pen_data.samples.empty()) {
      timestamp_us = std::max(timestamp_us, state.pen_data.samples.back().timestamp_us);
    }

    const auto canvas_width = std::max(1L, state.canvas.right - state.canvas.left - 2);
    const auto canvas_height = std::max(1L, state.canvas.bottom - state.canvas.top - 2);
    const auto normalized_x = std::clamp(
      static_cast<double>(point.x - state.canvas.left - 1) / canvas_width,
      0.0,
      1.0
    );
    const auto normalized_y = std::clamp(
      static_cast<double>(point.y - state.canvas.top - 1) / canvas_height,
      0.0,
      1.0
    );
    const auto pressure = (pen_info.penMask & PEN_MASK_PRESSURE) != 0 ?
                            std::clamp(static_cast<double>(pen_info.pressure) / pen_pressure_max, 0.0, 1.0) :
                            0.0;
    const auto rotation = (pen_info.penMask & PEN_MASK_ROTATION) != 0 ?
                            pen_info.rotation :
                            stylus_rotation_unknown;

    std::int32_t tilt = stylus_tilt_unknown;
    if (rotation != stylus_rotation_unknown &&
        (pen_info.penMask & PEN_MASK_TILT_X) != 0 &&
        (pen_info.penMask & PEN_MASK_TILT_Y) != 0) {
      const auto tilt_x = static_cast<double>(pen_info.tiltX) * std::numbers::pi / 180.0;
      const auto tilt_y = static_cast<double>(pen_info.tiltY) * std::numbers::pi / 180.0;
      const auto tangent = std::hypot(std::tan(tilt_x), std::tan(tilt_y));
      tilt = static_cast<std::int32_t>(std::clamp(
        std::lround(std::atan(tangent) * 180.0 / std::numbers::pi),
        0L,
        90L
      ));
    }

    state.pen_data.samples.push_back({
      timestamp_us,
      event_type,
      normalized_x,
      normalized_y,
      pressure,
      rotation,
      tilt,
    });
    const auto pending_samples = state.pen_data.samples.size() - state.recording_saved_samples;
    if (pending_samples >= recording_checkpoint_samples || event_type == stylus_event_up) {
      state.recording_checkpoint_requested = true;
    }
  }

  /** Preserve an interrupted contact stroke when Windows revokes pointer capture. */
  void
  record_pen_capture_cancellation(app_state_t &state) {
    if (!state.recording || !state.pen_in_contact || state.pen_data.samples.empty()) {
      return;
    }
    if (state.pen_data.samples.size() >= max_data_samples) {
      if (!state.pen_data.truncated) {
        state.pen_data.truncated = true;
        state.recording_checkpoint_requested = true;
        write_log_line(state, L"DATA | sample limit reached");
      }
      return;
    }

    auto sample = state.pen_data.samples.back();
    sample.event_type = stylus_event_cancel;
    sample.pressure = 0.0;
    state.pen_data.samples.push_back(sample);
    state.recording_checkpoint_requested = true;
  }

  /** Record one native pen sample. */
  void
  record_pen_sample(HWND window, app_state_t &state, UINT message, const POINTER_PEN_INFO &pen_info) {
    auto point = pen_info.pointerInfo.ptPixelLocation;
    ScreenToClient(window, &point);
    const auto in_contact = (pen_info.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) != 0;

    if (!point_in_canvas(state, point)) {
      // 画布外的移动不计入统计。接触中离开画布时记住轨迹断点，
      // 避免重新进入后把两段轨迹直接连线并统计跨画布的时间间隔。
      if (in_contact && state.pen_in_contact) {
        state.pen_trace_break_pending = true;
      }
      if (message == WM_POINTERUP && state.pen_in_contact) {
        // 录制只保留画布内坐标；用最后一个画布内样本的 CANCEL 关闭移出后抬起的笔划。
        record_pen_capture_cancellation(state);
      }
      if (message == WM_POINTERDOWN || message == WM_POINTERUP ||
          (message == WM_POINTERUPDATE && !in_contact) ||
          (message == WM_POINTERLEAVE && !in_contact)) {
        state.pen_in_contact = false;
        state.pen_trace_break_pending = false;
      }
      return;
    }

    // 录制文件保留画布内的悬停和接触样本；绘图与实时统计仍只处理接触事件。
    record_pen_data_sample(state, message, pen_info, point);

    if (message == WM_POINTERUPDATE && !in_contact) {
      state.pen_in_contact = false;
      return;
    }

    // 只有画布内的按下或接触移动才开启统计；悬停、画布外移动不记录。
    const auto starts_contact = message == WM_POINTERDOWN ||
                                (message == WM_POINTERUPDATE && in_contact && !state.pen_in_contact);
    const auto ends_contact = message == WM_POINTERUP && state.pen_in_contact;
    if (!state.pen_in_contact && !starts_contact && !ends_contact) {
      return;
    }

    state.last_pointer_id = pen_info.pointerInfo.pointerId;
    state.last_pen_point = point;
    state.last_pen_tick = GetTickCount64();

    const auto pressure_available = (pen_info.penMask & PEN_MASK_PRESSURE) != 0;
    state.last_pressure_available = pressure_available;
    if (pressure_available) {
      state.stats.pressure_seen = true;
      state.last_pressure = pen_info.pressure;
      state.stats.min_pressure = std::min(state.stats.min_pressure, pen_info.pressure);
      state.stats.max_pressure = std::max(state.stats.max_pressure, pen_info.pressure);
    }

    const auto tilt_available = (pen_info.penMask & PEN_MASK_TILT_X) != 0 &&
                                (pen_info.penMask & PEN_MASK_TILT_Y) != 0;
    state.last_tilt_available = tilt_available;
    if (tilt_available) {
      state.stats.tilt_seen = true;
      state.last_tilt_x = pen_info.tiltX;
      state.last_tilt_y = pen_info.tiltY;
      state.stats.min_tilt_x = std::min(state.stats.min_tilt_x, pen_info.tiltX);
      state.stats.max_tilt_x = std::max(state.stats.max_tilt_x, pen_info.tiltX);
      state.stats.min_tilt_y = std::min(state.stats.min_tilt_y, pen_info.tiltY);
      state.stats.max_tilt_y = std::max(state.stats.max_tilt_y, pen_info.tiltY);
    }

    const auto rotation_available = (pen_info.penMask & PEN_MASK_ROTATION) != 0;
    state.last_rotation_available = rotation_available;
    if (rotation_available) {
      state.stats.rotation_seen = true;
      state.last_rotation = pen_info.rotation;
      state.stats.min_rotation = std::min(state.stats.min_rotation, pen_info.rotation);
      state.stats.max_rotation = std::max(state.stats.max_rotation, pen_info.rotation);
    }

    bool pen_trace_updated = false;
    switch (message) {
      case WM_POINTERDOWN:
        ++state.stats.pen_down;
        state.pen_in_contact = true;
        state.pen_trace_break_pending = false;
        append_trace(state.pen_trace, {
          point,
          true,
          pen_info.pressure,
          pressure_available,
          pen_info.pointerInfo.dwTime,
          pen_info.pointerInfo.PerformanceCount,
        });
        pen_trace_updated = true;
        break;
      case WM_POINTERUP:
        ++state.stats.pen_up;
        if (state.pen_in_contact) {
          append_trace(state.pen_trace, {
            point,
            state.pen_trace_break_pending,
            pen_info.pressure,
            pressure_available,
            pen_info.pointerInfo.dwTime,
            pen_info.pointerInfo.PerformanceCount,
          });
          pen_trace_updated = true;
        }
        state.pen_in_contact = false;
        state.pen_trace_break_pending = false;
        break;
      case WM_POINTERUPDATE:
        if (in_contact) {
          ++state.stats.pen_update;
          append_trace(state.pen_trace, {
            point,
            state.pen_trace_break_pending || !state.pen_in_contact,
            pen_info.pressure,
            pressure_available,
            pen_info.pointerInfo.dwTime,
            pen_info.pointerInfo.PerformanceCount,
          });
          pen_trace_updated = true;
          state.pen_in_contact = true;
          state.pen_trace_break_pending = false;
        }
        break;
      case WM_POINTERENTER:
      case WM_POINTERLEAVE:
      case WM_POINTERCAPTURECHANGED:
        if (!in_contact) {
          state.pen_in_contact = false;
          state.pen_trace_break_pending = false;
        }
        break;
      default:
        break;
    }
    if (pen_trace_updated) {
      state.sampling_analysis_dirty = true;
    }
    state.canvas_dirty = true;
  }

  /** Record one native pen pointer message, including coalesced samples. */
  bool
  record_pen_message(HWND window, app_state_t &state, UINT message, WPARAM w_param, LPARAM l_param) {
    if (state.viewing_imported_data) {
      return true;
    }

    const auto pointer_id = GET_POINTERID_WPARAM(w_param);
    if (message == WM_POINTERCAPTURECHANGED && state.pen_in_contact &&
        pointer_id == state.last_pointer_id) {
      record_pen_capture_cancellation(state);
      std::wostringstream output;
      output << current_time_text()
             << L" | PEN_CAPTURE_CHANGED | pointerId=" << pointer_id;
      state.pen_event_seen = true;
      state.last_event = L"PEN_CAPTURE_CHANGED";
      write_log_line(state, output.str());
      state.pen_in_contact = false;
      state.pen_trace_break_pending = false;
      return true;
    }

    POINTER_INPUT_TYPE pointer_type {};
    if (!GetPointerType(pointer_id, &pointer_type) || pointer_type != PT_PEN) {
      return false;
    }

    if (message == WM_POINTERCAPTURECHANGED) {
      state.pen_in_contact = false;
      state.pen_trace_break_pending = false;
      return true;
    }

    POINTER_PEN_INFO pen_info {};
    if (!GetPointerPenInfo(pointer_id, &pen_info)) {
      const auto win32_error = GetLastError();
      POINT message_point {GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
      ScreenToClient(window, &message_point);
      if (message == WM_POINTERUP && state.pen_in_contact) {
        // 抬笔详情读取失败时也要关闭当前笔划，避免后续输入与未结束的旧笔划相连。
        record_pen_capture_cancellation(state);
        state.pen_in_contact = false;
        state.pen_trace_break_pending = false;
      }
      if (!point_in_canvas(state, message_point)) {
        return true;
      }
      if (message != WM_POINTERDOWN && !state.pen_in_contact) {
        return true;
      }
      ++state.stats.pen_errors;
      if (!state.pen_read_error_reported) {
        state.pen_read_error_reported = true;
        write_log_line(state,
          L"PEN_READ_ERROR | pointerId=" + std::to_wstring(pointer_id) +
            L" | win32Error=" + std::to_wstring(win32_error));
      }
      return true;
    }

    if (message == WM_POINTERUPDATE && pen_info.pointerInfo.historyCount > 1) {
      auto history_count = pen_info.pointerInfo.historyCount;
      if (history_count <= max_pointer_history_samples) {
        try {
          std::vector<POINTER_PEN_INFO> history(history_count);
          if (GetPointerPenInfoHistory(pointer_id, &history_count, history.data())) {
            // Win32 以新到旧返回历史；画布按旧到新绘制，避免轨迹反向。
            // 返回值可能报告比调用方缓冲区更多的可用项，只处理本次实际容纳的部分。
            const auto received_count = std::min<std::size_t>(history_count, history.size());
            for (auto index = received_count; index > 0; --index) {
              record_pen_sample(window, state, message, history[index - 1]);
            }
          }
          else {
            ++state.stats.pen_errors;
            if (!state.pen_history_error_reported) {
              state.pen_history_error_reported = true;
              write_log_line(state, L"PEN_HISTORY_READ_ERROR | win32Error=" + std::to_wstring(GetLastError()));
            }
            record_pen_sample(window, state, message, pen_info);
          }
        }
        catch (const std::exception &) {
          ++state.stats.pen_errors;
          if (!state.pen_history_allocation_error_reported) {
            state.pen_history_allocation_error_reported = true;
            write_log_line(state, L"PEN_HISTORY_ALLOCATION_ERROR");
          }
          record_pen_sample(window, state, message, pen_info);
        }
      }
      else {
        ++state.stats.pen_errors;
        if (!state.pen_history_limit_reported) {
          state.pen_history_limit_reported = true;
          write_log_line(state, L"PEN_HISTORY_LIMIT_EXCEEDED | samples=" + std::to_wstring(history_count));
        }
        record_pen_sample(window, state, message, pen_info);
      }
    }
    else {
      record_pen_sample(window, state, message, pen_info);
    }

    auto client_point = pen_info.pointerInfo.ptPixelLocation;
    ScreenToClient(window, &client_point);
    if (point_in_canvas(state, client_point)) {
      update_pen_status(state, message, pen_info, client_point);
    }

    return true;
  }

  /** Record one compatible mouse message. */
  bool
  record_mouse_message(app_state_t &state, UINT message, WPARAM w_param, LPARAM l_param) {
    if (state.viewing_imported_data) {
      return true;
    }

    POINT point {GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
    if (!point_in_canvas(state, point)) {
      // 按住左键离开画布时终止当前鼠标轨迹，重新进入后从新断点开始。
      state.mouse_in_contact = false;
      state.last_mouse_point_available = false;
      return false;
    }
    if (message == WM_MOUSEMOVE && state.last_mouse_point_available &&
        point.x == state.last_mouse_point.x && point.y == state.last_mouse_point.y) {
      return false;
    }
    if (message == WM_MOUSEMOVE) {
      state.last_mouse_point = point;
      state.last_mouse_point_available = true;
    }

    if (message == WM_MOUSEMOVE && (w_param & MK_LBUTTON) == 0) {
      state.mouse_in_contact = false;
      return false;
    }

    const auto extra_info = static_cast<std::uintptr_t>(GetMessageExtraInfo());
    const auto promoted = is_pen_promoted_mouse(extra_info);
    const auto correlated = promoted || is_correlated_mouse(state, point);
    if (promoted) {
      ++state.stats.promoted_mouse;
    }
    if (correlated) {
      ++state.stats.correlated_mouse;
    }
    if (state.filter_promoted_mouse && promoted) {
      ++state.stats.filtered_promoted_mouse;
      state.last_event = L"已过滤笔兼容鼠标消息";
      state.mouse_in_contact = false;
      state.canvas_dirty = true;
      return true;
    }

    const auto starts_contact = message == WM_LBUTTONDOWN ||
                                (message == WM_MOUSEMOVE && (w_param & MK_LBUTTON) != 0 && !state.mouse_in_contact);
    const auto ends_contact = message == WM_LBUTTONUP && state.mouse_in_contact;
    if (!state.mouse_in_contact && !starts_contact && !ends_contact) {
      return false;
    }

    update_mouse_status(state, message, point);
    switch (message) {
      case WM_LBUTTONDOWN:
        ++state.stats.mouse_down;
        state.mouse_in_contact = true;
        append_trace(state.mouse_trace, {point, true});
        break;
      case WM_LBUTTONUP:
        ++state.stats.mouse_up;
        if (state.mouse_in_contact) {
          append_trace(state.mouse_trace, {point, false});
        }
        state.mouse_in_contact = false;
        break;
      case WM_MOUSEMOVE:
        ++state.stats.mouse_move;
        append_trace(state.mouse_trace, {point, !state.mouse_in_contact});
        state.mouse_in_contact = true;
        break;
      default:
        break;
    }

    state.canvas_dirty = true;
    return false;
  }

  /** Handle the periodic reporting interval. */
  void
  publish_report(HWND window, app_state_t &state) {
    if (state.viewing_imported_data) {
      return;
    }

    if (state.sampling_analysis_dirty) {
      state.sampling_analysis = analyze_sampling(state.pen_trace);
      state.sampling_analysis_dirty = false;
    }
    state.last_report = make_report(state);
    state.stats = {};
    state.canvas_dirty = true;
    InvalidateRect(window, nullptr, FALSE);
  }

  /** Handle messages for the diagnostic window. */
  LRESULT CALLBACK
  window_proc_impl(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto state = reinterpret_cast<app_state_t *>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
      state = new app_state_t;
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
      state->recording_directory = make_recording_directory();
      state->log_path = make_log_path(state->recording_directory);
    }

    if (state == nullptr) {
      return DefWindowProcW(window, message, w_param, l_param);
    }

    switch (message) {
      case WM_CREATE: {
        state->dpi = get_window_dpi(window);
        state->clear_button = CreateWindowW(L"BUTTON", L"清空画布", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(clear_button_id), nullptr, nullptr);
        state->copy_button = CreateWindowW(L"BUTTON", L"复制报告", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(copy_button_id), nullptr, nullptr);
        state->import_data_button = CreateWindowW(L"BUTTON", L"导入数据", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(import_data_button_id), nullptr, nullptr);
        state->export_data_button = CreateWindowW(L"BUTTON", L"导出数据", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(export_data_button_id), nullptr, nullptr);
        state->record_data_button = CreateWindowW(L"BUTTON", L"开始录制", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(record_data_button_id), nullptr, nullptr);
        state->open_log_button = CreateWindowW(L"BUTTON", L"打开日志", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(open_log_button_id), nullptr, nullptr);
        state->copy_log_path_button = CreateWindowW(L"BUTTON", L"复制日志路径", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(copy_log_path_button_id), nullptr, nullptr);
        state->open_recording_folder_button = CreateWindowW(L"BUTTON", L"打开录制目录", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(open_recording_folder_button_id), nullptr, nullptr);
        state->filter_promoted_mouse_checkbox = CreateWindowW(L"BUTTON", L"过滤笔兼容鼠标消息（仅本工具）", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(filter_promoted_mouse_checkbox_id), nullptr, nullptr);
        if (state->clear_button == nullptr || state->copy_button == nullptr ||
            state->import_data_button == nullptr || state->export_data_button == nullptr ||
            state->record_data_button == nullptr || state->open_log_button == nullptr ||
            state->copy_log_path_button == nullptr || state->open_recording_folder_button == nullptr ||
            state->filter_promoted_mouse_checkbox == nullptr) {
          MessageBoxW(window, L"创建检测工具界面失败。", window_title, MB_OK | MB_ICONERROR);
          return -1;
        }
        update_ui_font(*state);
        update_layout(window, *state);
        update_recording_controls(*state);
        open_log(*state);
        if (SetTimer(window, report_timer_id, report_interval_ms, nullptr) == 0) {
          state->last_report = L"统计计时器启动失败，实时绘制仍可使用。";
          write_log_line(*state, L"TIMER | report timer failed");
        }
        if (SetTimer(window, repaint_timer_id, repaint_interval_ms, nullptr) == 0) {
          MessageBoxW(window, L"刷新计时器启动失败，检测工具无法继续运行。", window_title, MB_OK | MB_ICONERROR);
          return -1;
        }
        return 0;
      }
      case WM_SIZE:
        update_layout(window, *state, true);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      case WM_SETCURSOR:
        if (reinterpret_cast<HWND>(w_param) == window && LOWORD(l_param) == HTCLIENT) {
          POINT point {};
          if (GetCursorPos(&point) && ScreenToClient(window, &point)) {
            SetCursor(LoadCursorW(nullptr, point_in_canvas(*state, point) ? IDC_CROSS : IDC_ARROW));
            return TRUE;
          }
        }
        break;
      case WM_GETMINMAXINFO: {
        auto min_max = reinterpret_cast<MINMAXINFO *>(l_param);
        min_max->ptMinTrackSize = {
          scale_for_dpi(980, state->dpi),
          scale_for_dpi(700, state->dpi),
        };
        return 0;
      }
      case WM_DPICHANGED: {
        const auto new_dpi = static_cast<UINT>(LOWORD(w_param));
        scale_recorded_points(*state, state->dpi, new_dpi);
        state->dpi = new_dpi;
        update_ui_font(*state);

        const auto suggested_rect = reinterpret_cast<const RECT *>(l_param);
        SetWindowPos(
          window,
          nullptr,
          suggested_rect->left,
          suggested_rect->top,
          suggested_rect->right - suggested_rect->left,
          suggested_rect->bottom - suggested_rect->top,
          SWP_NOZORDER | SWP_NOACTIVATE
        );
        update_layout(window, *state);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      case WM_COMMAND:
        switch (LOWORD(w_param)) {
          case clear_button_id:
            clear_capture_state(*state);
            state->last_event = L"尚未收到检测区输入事件。";
            state->last_report = L"画布与本周期统计已清空。";
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          case copy_button_id:
            copy_text_to_clipboard(window, state->last_report);
            return 0;
          case open_log_button_id:
            ShellExecuteW(window, L"open", state->log_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
          case copy_log_path_button_id:
            if (copy_text_to_clipboard(window, state->log_path.wstring())) {
              state->last_event = L"已复制运行日志路径";
              state->canvas_dirty = true;
              InvalidateRect(window, nullptr, FALSE);
            }
            else {
              MessageBoxW(window, L"无法复制运行日志路径。", window_title, MB_OK | MB_ICONERROR);
            }
            return 0;
          case open_recording_folder_button_id: {
            std::wstring error;
            if (!ensure_recording_directory(state->recording_directory, error)) {
              MessageBoxW(window, error.c_str(), window_title, MB_OK | MB_ICONERROR);
              return 0;
            }
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", state->recording_directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
              MessageBoxW(window, L"无法打开录制目录。", window_title, MB_OK | MB_ICONERROR);
            }
            return 0;
          }
          case import_data_button_id:
            import_stylus_data(window, *state);
            return 0;
          case export_data_button_id:
            export_stylus_data(window, *state);
            return 0;
          case record_data_button_id:
            toggle_recording(window, *state);
            return 0;
          case filter_promoted_mouse_checkbox_id:
            state->filter_promoted_mouse = SendMessageW(state->filter_promoted_mouse_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
            state->mouse_in_contact = false;
            state->last_mouse_point_available = false;
            state->last_report = state->filter_promoted_mouse ?
                                          L"已开启兼容鼠标过滤：仅此工具的画布会忽略 Windows 为 PT_PEN 生成的鼠标消息。" :
                                          L"已关闭兼容鼠标过滤：画布会重新处理 Windows 为 PT_PEN 生成的鼠标消息。";
            write_log_line(*state, std::wstring(L"SETTING | filter promoted mouse=") + (state->filter_promoted_mouse ? L"enabled" : L"disabled"));
            state->canvas_dirty = true;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
          default:
            break;
        }
        break;
      case WM_TIMER:
        if (w_param == report_timer_id) {
          publish_report(window, *state);
          return 0;
        }
        if (w_param == repaint_timer_id) {
          if (state->recording && state->recording_checkpoint_requested) {
            std::wstring error;
            if (!checkpoint_recording_file(*state, error)) {
              handle_recording_checkpoint_failure(window, *state, error);
              return 0;
            }
          }
          if (state->canvas_dirty) {
            // 只分析最近的有界样本，让间隔、P95 和转向指标随画布刷新实时更新。
            if (state->sampling_analysis_dirty) {
              state->sampling_analysis = analyze_sampling(state->pen_trace);
              state->sampling_analysis_dirty = false;
            }
            state->canvas_dirty = false;
            // 状态、当前压力和采样图位于画布外，需要与画布一起重绘。
            InvalidateRect(window, nullptr, FALSE);
          }
          return 0;
        }
        break;
      case WM_POINTERDOWN:
      case WM_POINTERUP:
      case WM_POINTERUPDATE:
      case WM_POINTERENTER:
      case WM_POINTERLEAVE:
      case WM_POINTERCAPTURECHANGED:
        if (record_pen_message(window, *state, message, w_param, l_param)) {
          // 继续交给默认窗口过程，用于观察 Windows 生成的兼容鼠标消息。
          return DefWindowProcW(window, message, w_param, l_param);
        }
        break;
      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_MOUSEMOVE:
        if (record_mouse_message(*state, message, w_param, l_param)) {
          return 0;
        }
        break;
      case WM_PAINT:
        state->canvas_dirty = false;
        paint_window(window, *state);
        return 0;
      case WM_ERASEBKGND:
        return 1;
      case WM_CLOSE:
        if (state->recording) {
          // 保存失败会提示并保留内存数据，但不能让窗口陷入无法关闭的状态。
          stop_recording(window, *state, true);
        }
        DestroyWindow(window);
        return 0;
      case WM_DESTROY:
        KillTimer(window, report_timer_id);
        KillTimer(window, repaint_timer_id);
        state->log_writer.flush();
        PostQuitMessage(0);
        return 0;
      case WM_NCDESTROY: {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        const auto result = DefWindowProcW(window, message, w_param, l_param);
        if (state->ui_font != nullptr) {
          DeleteObject(state->ui_font);
        }
        delete state;
        return result;
      }
      default:
        break;
    }

    return DefWindowProcW(window, message, w_param, l_param);
  }

  /** Stop safely when an unexpected C++ exception reaches the Win32 callback boundary. */
  void
  handle_unexpected_exception(HWND window, UINT message, app_state_t *state) noexcept {
    if (state != nullptr && state->fatal_error) {
      if (window != nullptr && IsWindow(window) && message != WM_NCDESTROY) {
        DestroyWindow(window);
      }
      return;
    }

    if (state != nullptr && message != WM_NCDESTROY) {
      state->fatal_error = true;
      try {
        if (state->recording) {
          state->recording = false;
          std::wstring error;
          close_recording_file(*state, error);
        }
        write_log_line(*state, L"FATAL | unexpected exception");
        state->log_writer.flush();
      }
      catch (...) {
      }
    }

    if (window != nullptr && IsWindow(window)) {
      ShowWindow(window, SW_HIDE);
    }
    MessageBoxW(
      window,
      L"检测工具遇到异常并已停止。若当时正在录制，程序已尝试保存现有数据。",
      window_title,
      MB_OK | MB_ICONERROR
    );
    if (window != nullptr && IsWindow(window) && message != WM_NCDESTROY) {
      DestroyWindow(window);
    }
  }

  /** Keep C++ exceptions from escaping through the Win32 callback ABI. */
  LRESULT CALLBACK
  window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) noexcept {
    try {
      return window_proc_impl(window, message, w_param, l_param);
    }
    catch (const std::exception &) {
      auto state = message == WM_NCDESTROY ?
                     nullptr :
                     reinterpret_cast<app_state_t *>(GetWindowLongPtrW(window, GWLP_USERDATA));
      handle_unexpected_exception(window, message, state);
      return message == WM_NCCREATE ? FALSE : 0;
    }
    catch (...) {
      auto state = message == WM_NCDESTROY ?
                     nullptr :
                     reinterpret_cast<app_state_t *>(GetWindowLongPtrW(window, GWLP_USERDATA));
      handle_unexpected_exception(window, message, state);
      return message == WM_NCCREATE ? FALSE : 0;
    }
  }
}  // namespace

int WINAPI
wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  enable_per_monitor_dpi_awareness();

  WNDCLASSEXW window_class {
    .cbSize = sizeof(WNDCLASSEXW),
    .style = CS_HREDRAW | CS_VREDRAW,
    .lpfnWndProc = window_proc,
    .hInstance = instance,
    .hCursor = LoadCursorW(nullptr, IDC_ARROW),
    .hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1),
    .lpszClassName = window_class_name,
  };

  if (RegisterClassExW(&window_class) == 0) {
    MessageBoxW(nullptr, L"注册检测工具窗口失败。", window_title, MB_OK | MB_ICONERROR);
    return 1;
  }

  const auto initial_dpi = get_system_dpi();
  const auto window = CreateWindowExW(
    0,
    window_class_name,
    window_title,
    WS_OVERLAPPEDWINDOW,
    CW_USEDEFAULT,
    CW_USEDEFAULT,
    scale_for_dpi(980, initial_dpi),
    scale_for_dpi(720, initial_dpi),
    nullptr,
    nullptr,
    instance,
    nullptr
  );
  if (window == nullptr) {
    MessageBoxW(nullptr, L"创建检测工具窗口失败。", window_title, MB_OK | MB_ICONERROR);
    return 1;
  }

  ShowWindow(window, show_command);
  UpdateWindow(window);

  MSG message {};
  BOOL message_result {};
  while ((message_result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  if (message_result == -1) {
    MessageBoxW(nullptr, L"读取窗口消息失败，检测工具已停止。", window_title, MB_OK | MB_ICONERROR);
    return 1;
  }

  return static_cast<int>(message.wParam);
}
