/**
 * @file src/platform/windows/display_vdd.cpp
 * @brief VDD direct-capture backend.
 *
 * Opens the named D3D11 shared texture exported by the ZakoVDD driver
 * (SharedFrameExporter, see Virtual-Display-Driver/Virtual Display Driver (HDR)/
 * ZakoVDD/Driver.cpp). Bypasses DXGI Desktop Duplication / WGC entirely so it
 * works:
 *   - in SYSTEM service context (SunshineService)
 *   - before any user logs on
 *   - across user-switch / lock / RDP transitions
 *   - with full HDR (R10G10B10A2 / RGBA16F)
 * Limitation: only captures VDD virtual monitors, not physical displays.
 */

#include "display.h"
#include "misc.h"
#include "src/display_device/vdd_utils.h"
#include "src/main.h"

#include <d3d11_1.h>
#include <sddl.h>
#include <chrono>
#include <string>
#include <thread>

namespace platf {
  using namespace std::literals;
}

namespace platf::dxgi {

  // Must stay binary-compatible with the producer-side struct in
  // Virtual-Display-Driver/.../ZakoVDD/Driver.cpp (SharedFrameMetadata).
  struct SharedFrameMetadata {
    UINT32 Magic;            // 'ZVDF' = 0x5A564446
    UINT32 Version;          // 1
    UINT32 Width;
    UINT32 Height;
    UINT32 DxgiFormat;
    UINT32 IsHdr;
    float  MaxNits;
    float  MinNits;
    float  MaxFALL;
    UINT64 FrameCounter;
    UINT64 LastPresentQpc;
  };

  static constexpr UINT32 VDD_META_MAGIC = 0x5A564446;  // 'ZVDF'
  static constexpr UINT32 VDD_META_VERSION = 1;

  vdd_capture_t::vdd_capture_t() = default;

  vdd_capture_t::~vdd_capture_t() {
    close();
  }

  void
  vdd_capture_t::close() {
    if (m_holdsKey && m_keyedMutex) {
      m_keyedMutex->ReleaseSync(0);
      m_holdsKey = false;
    }
    m_keyedMutex.reset();
    m_sharedTex.reset();
    if (m_pMeta) {
      UnmapViewOfFile(m_pMeta);
      m_pMeta = nullptr;
    }
    if (m_hMeta) {
      CloseHandle(m_hMeta);
      m_hMeta = nullptr;
    }
    if (m_hEvent) {
      CloseHandle(m_hEvent);
      m_hEvent = nullptr;
    }
  }

  int
  vdd_capture_t::init(ID3D11Device *d3d_device, unsigned int monitor_idx) {
    if (!d3d_device) {
      BOOST_LOG(error) << "[vdd_capture] init: null D3D11 device"sv;
      return -1;
    }

    std::wstring meta_name = L"Global\\ZakoVDD_Meta_" + std::to_wstring(monitor_idx);
    std::wstring ev_name = L"Global\\ZakoVDD_FrameReady_" + std::to_wstring(monitor_idx);
    std::wstring tex_name = L"Global\\ZakoVDD_Frame_" + std::to_wstring(monitor_idx);

    // Open metadata first so we can fail fast if the driver isn't running
    // (or hasn't published a swap chain yet for this monitor).
    m_hMeta = OpenFileMappingW(FILE_MAP_READ, FALSE, meta_name.c_str());
    if (!m_hMeta) {
      auto err = GetLastError();
      BOOST_LOG(warning) << "[vdd_capture] OpenFileMappingW failed for monitor "sv
                         << monitor_idx << " (gle="sv << err << "). "sv
                         << "VDD driver running? Monitor active?"sv;
      return -1;
    }

    m_pMeta = MapViewOfFile(m_hMeta, FILE_MAP_READ, 0, 0, sizeof(SharedFrameMetadata));
    if (!m_pMeta) {
      BOOST_LOG(error) << "[vdd_capture] MapViewOfFile failed: "sv << GetLastError();
      close();
      return -1;
    }

    auto *meta = static_cast<const SharedFrameMetadata *>(m_pMeta);
    if (meta->Magic != VDD_META_MAGIC) {
      BOOST_LOG(error) << "[vdd_capture] bad metadata magic: 0x"sv
                       << util::hex(meta->Magic).to_string_view();
      close();
      return -1;
    }
    if (meta->Version != VDD_META_VERSION) {
      BOOST_LOG(error) << "[vdd_capture] metadata version mismatch: producer="sv
                       << meta->Version << " consumer="sv << VDD_META_VERSION;
      close();
      return -1;
    }

    m_width = meta->Width;
    m_height = meta->Height;
    m_format = static_cast<DXGI_FORMAT>(meta->DxgiFormat);
    m_is_hdr = (meta->IsHdr != 0);
    m_max_nits = meta->MaxNits;
    m_min_nits = meta->MinNits;
    m_max_fall = meta->MaxFALL;
    m_lastFrameCounter = meta->FrameCounter;

    if (m_width == 0 || m_height == 0) {
      BOOST_LOG(warning) << "[vdd_capture] producer has not pushed any frame yet "
                            "(width/height = 0). Monitor may be inactive."sv;
      // Not necessarily fatal — caller may retry — but at this point we have
      // no shared texture either, so report failure.
      close();
      return -1;
    }

    m_hEvent = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, ev_name.c_str());
    if (!m_hEvent) {
      BOOST_LOG(error) << "[vdd_capture] OpenEventW failed: "sv << GetLastError();
      close();
      return -1;
    }

    // Open the named shared texture using ID3D11Device1::OpenSharedResourceByName.
    ID3D11Device1 *dev1_p = nullptr;
    HRESULT hr = d3d_device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&dev1_p));
    if (FAILED(hr) || !dev1_p) {
      BOOST_LOG(error) << "[vdd_capture] ID3D11Device1 not available: 0x"sv
                       << util::hex(hr).to_string_view();
      close();
      return -1;
    }
    device1_t dev1{dev1_p};

    ID3D11Texture2D *raw = nullptr;
    hr = dev1->OpenSharedResourceByName(tex_name.c_str(),
                                        DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                        __uuidof(ID3D11Texture2D),
                                        reinterpret_cast<void **>(&raw));
    if (FAILED(hr) || !raw) {
      BOOST_LOG(error) << "[vdd_capture] OpenSharedResourceByName failed: 0x"sv
                       << util::hex(hr).to_string_view()
                       << ". LUID mismatch with VDD RenderAdapter?"sv;
      close();
      return -1;
    }
    m_sharedTex.reset(raw);

    IDXGIKeyedMutex *km = nullptr;
    hr = m_sharedTex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(&km));
    if (FAILED(hr) || !km) {
      BOOST_LOG(error) << "[vdd_capture] no IDXGIKeyedMutex on shared texture: 0x"sv
                       << util::hex(hr).to_string_view();
      close();
      return -1;
    }
    m_keyedMutex.reset(km);

    BOOST_LOG(info) << "[vdd_capture] opened monitor "sv << monitor_idx
                    << " "sv << m_width << "x"sv << m_height
                    << " fmt="sv << static_cast<int>(m_format)
                    << " hdr="sv << m_is_hdr;
    return 0;
  }

  capture_e
  vdd_capture_t::next_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out, uint64_t &out_frame_qpc) {
    if (out) *out = nullptr;
    out_frame_qpc = 0;

    if (!m_hEvent || !m_keyedMutex || !m_sharedTex || !m_pMeta) {
      return capture_e::error;
    }

    auto *meta = static_cast<const SharedFrameMetadata *>(m_pMeta);

    // Wait for the next frame-ready signal from the producer.
    DWORD ms = static_cast<DWORD>(timeout.count() < 0 ? 0 : timeout.count());
    DWORD wr = WaitForSingleObject(m_hEvent, ms);
    if (wr == WAIT_TIMEOUT) {
      return capture_e::timeout;
    }
    if (wr != WAIT_OBJECT_0) {
      BOOST_LOG(error) << "[vdd_capture] WaitForSingleObject: gle="sv << GetLastError();
      return capture_e::error;
    }

    // Producer released as key 1; consumer acquires key 1.
    HRESULT hr = m_keyedMutex->AcquireSync(1, ms);
    if (hr == static_cast<HRESULT>(WAIT_TIMEOUT)) {
      return capture_e::timeout;
    }
    if (FAILED(hr)) {
      BOOST_LOG(error) << "[vdd_capture] AcquireSync(1) failed: 0x"sv
                       << util::hex(hr).to_string_view();
      return capture_e::error;
    }
    m_holdsKey = true;

    // Detect producer-side resize / format change: the metadata block can change
    // any time the swap chain is re-created. If so, signal reinit so the upper
    // layer reopens the shared texture.
    if (meta->Width != m_width || meta->Height != m_height ||
        static_cast<DXGI_FORMAT>(meta->DxgiFormat) != m_format) {
      BOOST_LOG(info) << "[vdd_capture] producer resolution/format changed, requesting reinit"sv;
      m_keyedMutex->ReleaseSync(0);
      m_holdsKey = false;
      return capture_e::reinit;
    }

    // Refresh HDR metadata (cheap copy from shared mapping).
    m_is_hdr = (meta->IsHdr != 0);
    m_max_nits = meta->MaxNits;
    m_min_nits = meta->MinNits;
    m_max_fall = meta->MaxFALL;
    m_lastFrameCounter = meta->FrameCounter;
    out_frame_qpc = meta->LastPresentQpc;

    if (out) {
      m_sharedTex->AddRef();
      *out = m_sharedTex.get();
    }
    return capture_e::ok;
  }

  capture_e
  vdd_capture_t::release_frame() {
    if (m_holdsKey && m_keyedMutex) {
      m_keyedMutex->ReleaseSync(0);
      m_holdsKey = false;
    }
    return capture_e::ok;
  }

  // ===========================================================================
  // display_vdd_vram_t
  // ===========================================================================
  //
  // Resolves the VDD monitor index for a given DXGI display by probing
  // Global\ZakoVDD_Meta_<i> mappings and matching producer-side dimensions
  // against the DXGI output's reported size. Then opens the shared texture
  // on the same D3D11 device as display_base_t to avoid cross-device copies.

  static int
  resolve_vdd_monitor_index(unsigned int target_w, unsigned int target_h, unsigned int max_probe = 16) {
    for (unsigned int i = 0; i < max_probe; ++i) {
      std::wstring meta_name = L"Global\\ZakoVDD_Meta_" + std::to_wstring(i);
      HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, meta_name.c_str());
      if (!h) continue;
      void *p = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(SharedFrameMetadata));
      if (!p) {
        CloseHandle(h);
        continue;
      }
      auto *meta = static_cast<const SharedFrameMetadata *>(p);
      bool match = (meta->Magic == VDD_META_MAGIC) &&
                   (meta->Width == target_w) &&
                   (meta->Height == target_h);
      UnmapViewOfFile(p);
      CloseHandle(h);
      if (match) {
        BOOST_LOG(info) << "[vdd] resolved monitor index "sv << i
                        << " for "sv << target_w << "x"sv << target_h;
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  int
  display_vdd_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name)) {
      BOOST_LOG(error) << "[vdd] display_base_t::init failed for "sv << display_name;
      return -1;
    }

    // Try to identify which VDD monitor backs this DXGI output by matching
    // dimensions. width/height come from DXGI DesktopCoordinates / orientation.
    int idx = resolve_vdd_monitor_index(static_cast<unsigned>(width_before_rotation),
                                        static_cast<unsigned>(height_before_rotation));
    if (idx < 0) {
      // Self-check: the selected display either isn't a VDD monitor at all, or
      // VDD has no active monitors yet. Try to ask the driver to create one
      // via its NamedPipe, then poll the meta mapping again.
      BOOST_LOG(info) << "[vdd] no matching VDD monitor; sending CREATEMONITOR via NamedPipe..."sv;
      std::string response;
      bool timed_out = false;
      bool sent = display_device::vdd_utils::execute_pipe_command(
        display_device::vdd_utils::kVddPipeName, L"CREATEMONITOR", &response, &timed_out);
      if (!sent) {
        BOOST_LOG(warning) << "[vdd] CREATEMONITOR failed (driver not running or pipe unreachable). "sv
                           << "Is the selected display a ZakoVDD virtual display?"sv;
        return -1;
      }
      // Poll for up to ~3s for a producer to appear.
      using namespace std::chrono_literals;
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(200ms);
        idx = resolve_vdd_monitor_index(static_cast<unsigned>(width_before_rotation),
                                        static_cast<unsigned>(height_before_rotation));
        if (idx >= 0) break;
      }
      if (idx < 0) {
        BOOST_LOG(warning) << "[vdd] CREATEMONITOR succeeded but no monitor matches "sv
                           << width_before_rotation << "x"sv << height_before_rotation
                           << " within 3s."sv;
        return -1;
      }
    }
    monitor_idx = static_cast<unsigned int>(idx);

    if (dup.init(device.get(), monitor_idx) != 0) {
      BOOST_LOG(error) << "[vdd] vdd_capture_t::init failed for monitor "sv << monitor_idx;
      return -1;
    }

    // Use producer-reported format as our capture format. complete_img() / image
    // pool will be created against this format.
    capture_format = dup.format();

    // Validate the producer-reported format against the formats display_vram_t
    // can actually consume (RTV creation, shaders, color conversion). Anything
    // outside this whitelist would crash later in capture/encoding paths.
    {
      auto supported = get_supported_capture_formats();
      bool ok = false;
      for (auto f : supported) {
        if (f == capture_format) { ok = true; break; }
      }
      if (!ok) {
        BOOST_LOG(error) << "[vdd] producer format "sv
                         << dxgi_format_to_string(capture_format)
                         << " is not in display_vram_t::get_supported_capture_formats(); "sv
                         << "rejecting. Extending RTV/shader paths is required to add it."sv;
        return -1;
      }
    }

    BOOST_LOG(info) << "[vdd] backend ready: monitor="sv << monitor_idx
                    << " "sv << dup.width() << "x"sv << dup.height()
                    << " fmt="sv << dxgi_format_to_string(capture_format)
                    << " hdr="sv << dup.is_hdr();
    return 0;
  }

  bool
  display_vdd_vram_t::is_hdr() {
    return dup.is_hdr();
  }

  bool
  display_vdd_vram_t::get_hdr_metadata(SS_HDR_METADATA &metadata) {
    std::memset(&metadata, 0, sizeof(metadata));
    if (!dup.is_hdr()) {
      return false;
    }

    // Report Rec. 2020 primaries with D65 white point. Mirrors
    // display_base_t::get_hdr_metadata(): the actual primaries depend on
    // shader-side conversion (scRGB FP16 -> PQ in Rec. 2020), so reporting
    // 2020 is the safe / consistent choice. Most clients only consume the
    // luminance fields anyway.
    metadata.displayPrimaries[0].x = 0.708f * 50000;
    metadata.displayPrimaries[0].y = 0.292f * 50000;
    metadata.displayPrimaries[1].x = 0.170f * 50000;
    metadata.displayPrimaries[1].y = 0.797f * 50000;
    metadata.displayPrimaries[2].x = 0.131f * 50000;
    metadata.displayPrimaries[2].y = 0.046f * 50000;
    metadata.whitePoint.x = 0.3127f * 50000;
    metadata.whitePoint.y = 0.3290f * 50000;

    // Producer-reported luminance, in nits. SS_HDR_METADATA expects:
    //   maxDisplayLuminance      : nits
    //   minDisplayLuminance      : units of 0.0001 nits
    //   maxFullFrameLuminance    : nits
    metadata.maxDisplayLuminance = static_cast<uint16_t>(dup.max_nits());
    metadata.minDisplayLuminance = static_cast<uint32_t>(dup.min_nits() * 10000.0f);
    metadata.maxFullFrameLuminance = static_cast<uint16_t>(dup.max_fall());

    // Producer doesn't currently track per-frame content light levels.
    metadata.maxContentLightLevel = 0;
    metadata.maxFrameAverageLightLevel = 0;
    return true;
  }

  // NOTE: snapshot() and release_snapshot() are implemented in display_vram.cpp,
  // alongside display_amd_vram_t / display_wgc_vram_t, because they need access
  // to the file-local types `img_d3d_t` and `texture_lock_helper`.

}  // namespace platf::dxgi
