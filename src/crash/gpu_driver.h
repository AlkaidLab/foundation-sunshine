/**
 * @file src/crash/gpu_driver.h
 * @brief Which graphics driver a crash or a hang ended in, and what the crash handler writes about it.
 *
 * A fatal call or a stuck thread whose top frame lies in a module of a graphics driver (Intel igd*, igxe*; NVIDIA nvwgf2um*, nvd3dum*, nvEncodeAPI*; AMD amdxx*, atiu*)
 * is not a fault of the host: the driver's own code failed. The crash handler leaves a small mark file for that, next to the reports, and the next start can say so
 * (src/gpu_driver_guard.h). The mark holds the vendor, the module file name, the driver version, the phase and the kind: no path, no name, no address.
 *
 * Everything the handler calls here is constexpr or works on caller-supplied fixed buffers: no allocation, no lock, no call into the CRT that may take one.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace crash::gpu_driver {

  enum class vendor_e : int { none = 0, intel = 1, nvidia = 2, amd = 3 };

  /// What the process was doing: it decides the sentence the person reads ("at start", "while checking the encoders", "during a stream").
  enum class phase_e : int { startup = 0, probe = 1, idle = 2, streaming = 3 };

  inline constexpr std::size_t module_cap = 64;
  inline constexpr std::size_t version_cap = 32;
  inline constexpr std::size_t mark_buffer_size = 512;
  inline constexpr const char *mark_file_name = "gpu_driver_mark.json";  ///< in <config>/crashes, next to pending/

  constexpr char
  lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
  }

  constexpr bool
  starts_with_i(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
      if (lower_ascii(s[i]) != prefix[i]) return false;  // prefixes below are lower case
    }
    return true;
  }

  constexpr bool
  contains_i(std::string_view s, std::string_view needle) {
    if (needle.empty() || s.size() < needle.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= s.size(); ++i) {
      bool same = true;
      for (std::size_t k = 0; k < needle.size() && same; ++k) same = lower_ascii(s[i + k]) == needle[k];
      if (same) return true;
    }
    return false;
  }

  /// The part after the last directory separator: a module is named by its file, never by where it lies (a path holds the account name).
  constexpr std::string_view
  base_name(std::string_view module) {
    for (std::size_t i = module.size(); i > 0; --i) {
      if (module[i - 1] == '/' || module[i - 1] == '\\') return module.substr(i);
    }
    return module;
  }

  namespace detail {
    // User-mode driver modules, lower case prefixes of the file name. Windows names come from the driver packages (DriverStore) and from System32; the Linux ones
    // are the Mesa and vendor libraries a stream loads. Everything here is a module that ships with the driver, never one an application bundles.
    inline constexpr std::string_view intel_prefixes[] = {
      "igd",       // igd10iumd64, igd12umd64, igdumdim64, igdrcl64, igdgmm64, igdfcl64, igdml64, igdext64
      "igxe",      // igxelpicd64, igxeumd64 (Xe, Arc)
      "igc",       // igc64 (graphics compiler)
      "igvk",      // igvk64 (Vulkan)
      "ig9icd",    // ig9icd64
      "igfx",      // igfxext64, igfxcmrt64
      "ze_intel",  // Level Zero
      "libmfxhw",  // Media SDK runtime
      "mfxplugin",  //
      "intelocl",  //
      "libigd",    //
      "libigc",    //
      "libvulkan_intel",
      "libze_intel",
      "ihd_drv_video",  // libva iHD
      "i965_drv_video",
      "iris_dri",
      "i965_dri",
    };
    inline constexpr std::string_view nvidia_prefixes[] = {
      "nvwgf2um",  // nvwgf2um, nvwgf2umx (the D3D10/11/12 user-mode driver)
      "nvd3d",     // nvd3dum, nvd3dumx
      "nvencodeapi",
      "nvencmft",
      "nvcuvid",
      "nvcuda",
      "nvapi",
      "nvldumd",
      "nvoglv",
      "nvopencl",
      "nvml",
      "nvofapi",
      "nvgpucomp",
      "libnvidia-",
      "libcuda.",
      "libnvcuvid",
      "libnvoptix",
      "nvidia_drv",
      "nvidia_dri",
      "nvidia_icd",
      "libglx_nvidia",
      "libegl_nvidia",
      "libglesv2_nvidia",
      "libgles1_nvidia",
    };
    inline constexpr std::string_view amd_prefixes[] = {
      "amdxx",     // amdxx64 (D3D11/12 user-mode driver)
      "amdxc",     // amdxc64 (shader compiler)
      "amdxn",     // amdxn64
      "amdxe",     //
      "atiu9p",    // atiu9p64
      "atiumd",    // atiumd64, atiumdag
      "atiuxp",    //
      "atidxx",    // atidxx64
      "aticfx",    // aticfx64
      "atiadlxx",  // atiadlxx
      "atig6p",    //
      "atioglxx",  //
      "atiglpxx",  //
      "amdvlk",    // amdvlk64 (Vulkan)
      "amdocl",    // amdocl64
      "amfrt",     // amfrt64 (the encoder runtime)
      "amdihk",    // amdihk64
      "amdgpu_dri",
      "radeonsi_dri",
      "r600_dri",
      "libvulkan_radeon",
      "libamdvlk",
      "libdrm_amdgpu",
      "libamfrt",
    };

    template <std::size_t N>
    constexpr bool
    any_prefix(std::string_view name, const std::string_view (&list)[N]) {
      for (const auto prefix : list) {
        if (starts_with_i(name, prefix)) return true;
      }
      return false;
    }

    /// A module the tests (and the end-to-end tool) declare as a driver, so a crash inside the host's own code can stand in for one: the env variable
    /// SUNSHINE_TEST_DRIVER_MODULE=<file name>=<intel|nvidia|amd>, read at the start of the crash handlers. Never set in a real run.
    struct test_module_t {
      char name[module_cap] = {};
      std::atomic<int> vendor { 0 };
    };
    inline test_module_t g_test_module;
  }  // namespace detail

  /// The vendor of the graphics driver a module file name belongs to, `none` for any other module (the host, the runtime, Windows, Mesa-independent libraries).
  constexpr vendor_e
  vendor_of_module_static(std::string_view module) {
    const auto name = base_name(module);
    if (name.empty()) return vendor_e::none;
    if (detail::any_prefix(name, detail::intel_prefixes)) return vendor_e::intel;
    if (detail::any_prefix(name, detail::nvidia_prefixes) || contains_i(name, "_nvidia.so")) return vendor_e::nvidia;
    if (detail::any_prefix(name, detail::amd_prefixes)) return vendor_e::amd;
    return vendor_e::none;
  }

  inline vendor_e
  vendor_of_module(std::string_view module) {
    if (const auto v = vendor_of_module_static(module); v != vendor_e::none) return v;
    const int test_vendor = detail::g_test_module.vendor.load(std::memory_order_acquire);
    if (test_vendor != 0) {
      const auto name = base_name(module);
      const std::string_view wanted { detail::g_test_module.name };
      if (name.size() == wanted.size() && !wanted.empty()) {
        bool same = true;
        for (std::size_t i = 0; i < name.size() && same; ++i) same = lower_ascii(name[i]) == lower_ascii(wanted[i]);
        if (same) return static_cast<vendor_e>(test_vendor);
      }
    }
    return vendor_e::none;
  }

  /// Declares `name` (a module file name) a driver of `vendor` for this process: tests only (see detail::test_module_t).
  inline void
  set_test_module(std::string_view name, vendor_e vendor) {
    detail::g_test_module.vendor.store(0, std::memory_order_release);
    std::size_t n = 0;
    for (; n < name.size() && n + 1 < module_cap; ++n) detail::g_test_module.name[n] = name[n];
    detail::g_test_module.name[n] = 0;
    detail::g_test_module.vendor.store(static_cast<int>(vendor), std::memory_order_release);
  }

  constexpr const char *
  vendor_word(vendor_e v) {
    switch (v) {
      case vendor_e::intel: return "intel";
      case vendor_e::nvidia: return "nvidia";
      case vendor_e::amd: return "amd";
      default: return "";
    }
  }

  constexpr vendor_e
  vendor_from_word(std::string_view word) {
    if (word == "intel") return vendor_e::intel;
    if (word == "nvidia") return vendor_e::nvidia;
    if (word == "amd") return vendor_e::amd;
    return vendor_e::none;
  }

  constexpr const char *
  phase_word(phase_e p) {
    switch (p) {
      case phase_e::startup: return "startup";
      case phase_e::probe: return "probe";
      case phase_e::idle: return "idle";
      case phase_e::streaming: return "streaming";
    }
    return "startup";
  }

  constexpr phase_e
  phase_from_word(std::string_view word) {
    if (word == "probe") return phase_e::probe;
    if (word == "idle") return phase_e::idle;
    if (word == "streaming") return phase_e::streaming;
    return phase_e::startup;
  }

  // --- what the handler reads: the phase and the driver versions, written by the host (plain atomics and fixed buffers) ---------------------------------

  struct facts_t {
    std::atomic<int> phase { 0 };
    char driver[4][version_cap] = {};  ///< indexed by vendor_e; the version the host read from the system at start
    std::atomic<bool> driver_ready[4] = {};  ///< the bytes of the version are complete (released after they are written; the handler reads them only then)
  };
  inline facts_t g_facts;

  inline void
  set_phase(phase_e p) noexcept {
    g_facts.phase.store(static_cast<int>(p), std::memory_order_release);
  }

  inline phase_e
  current_phase() noexcept {
    const int v = g_facts.phase.load(std::memory_order_acquire);
    return v >= 0 && v <= 3 ? static_cast<phase_e>(v) : phase_e::startup;
  }

  /// Version text: letters, digits and . - _ + only, at most 31 characters (the same rule as the machine description of a crash report).
  inline void
  set_driver_version(vendor_e vendor, std::string_view version) noexcept {
    if (vendor == vendor_e::none) return;
    const auto index = static_cast<int>(vendor);
    char text[version_cap] = {};
    std::size_t n = 0;
    for (const char c : version) {
      if (n + 1 >= version_cap) break;
      const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_' || c == '+';
      if (ok) text[n++] = c;
    }
    // the host sets each version once, from one thread (a later call only replaces it in tests): the handler never sees a half-written one
    g_facts.driver_ready[index].store(false, std::memory_order_release);
    for (std::size_t i = 0; i < version_cap; ++i) g_facts.driver[index][i] = text[i];
    g_facts.driver_ready[index].store(true, std::memory_order_release);
  }

  inline const char *
  driver_version_of(vendor_e vendor) noexcept {
    if (vendor == vendor_e::none) return "";
    const auto index = static_cast<int>(vendor);
    return g_facts.driver_ready[index].load(std::memory_order_acquire) ? g_facts.driver[index] : "";
  }

  // --- the mark ----------------------------------------------------------------------------------------------------------------------------------

  /// "crash" (a fatal exception, abort, terminate) or "hang" (a watched thread stayed busy too long).
  struct mark_t {
    vendor_e vendor = vendor_e::none;
    char module[module_cap] = {};
    char driver[version_cap] = {};
    phase_e phase = phase_e::startup;
    char kind[8] = {};
    std::int64_t time = 0;  ///< unix seconds
  };

  namespace detail {
    struct text_writer_t {
      char *out;
      std::size_t cap;
      std::size_t len = 0;
      void
      put(char c) {
        if (len + 1 < cap) out[len++] = c;
      }
      void
      raw(const char *s) {
        while (*s) put(*s++);
      }
      /// A JSON string of a short token: anything outside [A-Za-z0-9._+-] becomes '_'.
      void
      token(const char *s) {
        put('"');
        for (; s && *s; ++s) {
          const char c = *s;
          const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_' || c == '+';
          put(ok ? c : '_');
        }
        put('"');
      }
      void
      dec(std::uint64_t v) {
        char tmp[24];
        int n = 0;
        do {
          tmp[n++] = static_cast<char>('0' + v % 10);
          v /= 10;
        } while (v);
        while (n) put(tmp[--n]);
      }
    };
  }  // namespace detail

  /// `{"v":1,"vendor":"intel","module":"igd12umd64.dll","driver":"32.0.101.6325","phase":"startup","kind":"crash","time":1731889200}`; returns the length, 0 when it does not fit.
  inline std::size_t
  render_mark(const mark_t &mark, char *out, std::size_t cap) {
    if (cap < 8) return 0;
    detail::text_writer_t w { out, cap };
    w.raw("{\"v\":1,\"vendor\":");
    w.token(vendor_word(mark.vendor));
    w.raw(",\"module\":");
    w.token(mark.module);
    w.raw(",\"driver\":");
    w.token(mark.driver);
    w.raw(",\"phase\":");
    w.token(phase_word(mark.phase));
    w.raw(",\"kind\":");
    w.token(mark.kind);
    w.raw(",\"time\":");
    w.dec(static_cast<std::uint64_t>(mark.time < 0 ? 0 : mark.time));
    w.put('}');
    if (w.len + 1 >= cap) return 0;
    out[w.len] = 0;
    return w.len;
  }

  /// Fills a mark for a module found at the top of a stack: the vendor, the file name, the driver version the host knows for that vendor and the phase now.
  inline mark_t
  make_mark(vendor_e vendor, std::string_view module, const char *kind, std::int64_t time) {
    mark_t mark;
    mark.vendor = vendor;
    const auto name = base_name(module);
    std::size_t n = 0;
    for (; n < name.size() && n + 1 < module_cap; ++n) mark.module[n] = name[n];
    mark.module[n] = 0;
    const char *version = driver_version_of(vendor);
    for (n = 0; version[n] && n + 1 < version_cap; ++n) mark.driver[n] = version[n];
    mark.driver[n] = 0;
    mark.phase = current_phase();
    // the report kinds crash, abort and panic are all "crash" here: the process died; "hang" is a thread that stopped
    const bool hang = kind && kind[0] == 'h' && kind[1] == 'a' && kind[2] == 'n' && kind[3] == 'g' && kind[4] == 0;
    const char *word = hang ? "hang" : "crash";
    for (n = 0; word[n]; ++n) mark.kind[n] = word[n];
    mark.kind[n] = 0;
    mark.time = time;
    return mark;
  }

}  // namespace crash::gpu_driver
