/**
 * @file src/platform/windows/system_load_win.cpp
 * @brief Windows sampling for the system load route.
 *
 * CPU: GetSystemTimes (kernel32), memory: GlobalMemoryStatusEx, GPU: the PDH counter
 * `\GPU Engine(*)\Utilization Percentage` added in English (PdhAddEnglishCounterW), adapter
 * name from DXGI. Every call is non-blocking; rate-based values need two samples and are
 * null until the second one exists.
 */
#include "src/system_load.h"

// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
// clang-format on

#include <chrono>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace system_load {

  namespace {
    using steady_t = std::chrono::steady_clock;

    // A second call this soon returns the previous value: counters need a measurable interval.
    constexpr auto min_interval = std::chrono::milliseconds(250);

    std::uint64_t
    to_ticks(const FILETIME &time) {
      return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    }

    std::optional<double>
    sample_cpu() {
      static std::mutex mutex;
      static std::optional<cpu_times_t> previous;
      static steady_t::time_point previous_at;
      static std::optional<double> last;

      std::lock_guard lock { mutex };

      FILETIME idle_time, kernel_time, user_time;
      if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        return last;
      }
      const cpu_times_t now { to_ticks(idle_time), to_ticks(kernel_time), to_ticks(user_time) };
      const auto now_at = steady_t::now();

      if (!previous) {
        previous = now;
        previous_at = now_at;
        return last;
      }
      if (now_at - previous_at < min_interval) {
        return last;
      }

      if (const auto percent = cpu_percent_from_times(*previous, now)) {
        last = percent;
      }
      previous = now;
      previous_at = now_at;
      return last;
    }

    std::string
    utf8_from_wide(const wchar_t *text, int length) {
      if (length <= 0) {
        return {};
      }
      const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
      if (size <= 0) {
        return {};
      }
      std::string out(static_cast<std::size_t>(size), '\0');
      WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), size, nullptr, nullptr);
      return out;
    }

    // Adapter description for a LUID; looked up once per adapter and kept.
    std::optional<std::string>
    adapter_name(std::uint32_t luid_high, std::uint32_t luid_low) {
      static std::mutex mutex;
      static std::map<std::pair<std::uint32_t, std::uint32_t>, std::optional<std::string>> cache;

      std::lock_guard lock { mutex };
      const auto key = std::make_pair(luid_high, luid_low);
      if (const auto found = cache.find(key); found != cache.end()) {
        return found->second;
      }

      std::optional<std::string> name;
      IDXGIFactory1 *factory = nullptr;
      if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) && factory) {
        IDXGIAdapter1 *adapter = nullptr;
        for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
          DXGI_ADAPTER_DESC1 desc {};
          const bool have_desc = SUCCEEDED(adapter->GetDesc1(&desc));
          adapter->Release();
          adapter = nullptr;
          if (have_desc && static_cast<std::uint32_t>(desc.AdapterLuid.HighPart) == luid_high && desc.AdapterLuid.LowPart == luid_low) {
            name = utf8_from_wide(desc.Description, static_cast<int>(wcsnlen(desc.Description, 128)));
            break;
          }
        }
        factory->Release();
      }
      if (name && name->empty()) {
        name.reset();
      }
      // A failed lookup is not cached so a driver that appears later is still found.
      if (name) {
        cache[key] = name;
      }
      return name;
    }

    struct gpu_reading_t {
      std::optional<double> percent;
      std::optional<std::string> name;
    };

    struct gpu_query_t {
      std::mutex mutex;
      PDH_HQUERY query = nullptr;
      PDH_HCOUNTER counter = nullptr;
      bool init_attempted = false;
      steady_t::time_point last_at;
      bool have_last_at = false;
      gpu_reading_t last;
    };

    gpu_query_t &
    gpu_query() {
      // Never destroyed: the query lives as long as the process.
      static gpu_query_t *state = new gpu_query_t;
      return *state;
    }

    bool
    open_gpu_query(gpu_query_t &state) {
      if (state.counter) {
        return true;
      }
      if (state.init_attempted) {
        return false;
      }
      state.init_attempted = true;

      if (PdhOpenQueryW(nullptr, 0, &state.query) != ERROR_SUCCESS) {
        state.query = nullptr;
        return false;
      }
      if (PdhAddEnglishCounterW(state.query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &state.counter) != ERROR_SUCCESS) {
        PdhCloseQuery(state.query);
        state.query = nullptr;
        state.counter = nullptr;
        return false;
      }
      // First collection primes the rate counters; formatting needs the second one.
      PdhCollectQueryData(state.query);
      return true;
    }

    std::optional<std::vector<gpu_engine_sample_t>>
    read_gpu_engines(gpu_query_t &state) {
      if (PdhCollectQueryData(state.query) != ERROR_SUCCESS) {
        return std::nullopt;
      }

      // Instances come and go between the sizing call and the data call, so retry a few times.
      std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> items;
      DWORD bytes = 0;
      DWORD count = 0;
      PDH_STATUS status = PdhGetFormattedCounterArrayW(state.counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr);
      for (int attempt = 0; status == PDH_MORE_DATA && attempt < 3; ++attempt) {
        items.resize((bytes + sizeof(PDH_FMT_COUNTERVALUE_ITEM_W) - 1) / sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
        DWORD capacity = static_cast<DWORD>(items.size() * sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
        status = PdhGetFormattedCounterArrayW(state.counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &capacity, &count, items.data());
        if (status == PDH_MORE_DATA) {
          bytes = capacity;
        }
      }
      if (status != ERROR_SUCCESS) {
        return std::nullopt;
      }

      std::vector<gpu_engine_sample_t> samples;
      samples.reserve(count);
      for (DWORD i = 0; i < count; ++i) {
        const auto &item = items[i];
        if (!item.szName || (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)) {
          continue;
        }
        samples.push_back({ utf8_from_wide(item.szName, static_cast<int>(wcslen(item.szName))), item.FmtValue.doubleValue });
      }
      return samples;
    }

    gpu_reading_t
    sample_gpu() {
      auto &state = gpu_query();
      std::lock_guard lock { state.mutex };

      if (!open_gpu_query(state)) {
        return {};
      }

      const auto now = steady_t::now();
      if (state.have_last_at && now - state.last_at < min_interval) {
        return state.last;
      }
      state.last_at = now;
      state.have_last_at = true;

      const auto samples = read_gpu_engines(state);
      if (!samples) {
        return state.last;
      }

      const auto load = aggregate_gpu_adapters(*samples);
      if (!load) {
        state.last = {};
        return state.last;
      }
      state.last.percent = load->percent;
      state.last.name = adapter_name(load->luid_high, load->luid_low);
      return state.last;
    }
  }  // namespace

  snapshot_t
  sample() {
    snapshot_t out;
    out.supported = true;
    out.cpu_percent = sample_cpu();

    MEMORYSTATUSEX memory {};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
      out.memory_total = static_cast<std::uint64_t>(memory.ullTotalPhys);
      out.memory_available = static_cast<std::uint64_t>(memory.ullAvailPhys);
      out.memory_percent = memory_percent_from_bytes(memory.ullTotalPhys, memory.ullAvailPhys);
      if (!out.memory_percent) {
        out.memory_percent = static_cast<double>(memory.dwMemoryLoad);
      }
    }

    const auto gpu = sample_gpu();
    out.gpu_percent = gpu.percent;
    out.gpu_name = gpu.name;
    return out;
  }

}  // namespace system_load
