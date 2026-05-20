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
#include "src/main.h"

#include <d3d11_1.h>
#include <sddl.h>
#include <algorithm>
#include <chrono>
#include <cmath>
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

  // Mirror of CursorSharedMetadata in ZakoVDD/Driver.cpp. Layout is
  // 4-byte aligned (#pragma pack(push, 4) on the producer side); the
  // standard ABI on x64 already aligns the fields below identically.
  struct CursorSharedMetadata {
    UINT32 Magic;                // 'ZVCU' = 0x5A564355
    UINT32 Version;              // 1
    UINT32 IsVisible;            // 0/1
    INT32  PositionX;            // top-left of cursor image (already hot-spot adjusted, DXGI semantics)
    INT32  PositionY;
    UINT32 PositionId;           // monotonic on position change
    UINT32 ShapeId;              // monotonic on shape change
    UINT32 ShapeType;            // IDDCX_CURSOR_SHAPE_TYPE value (0=mono, 1=color, 2=masked color)
    UINT32 Width;
    UINT32 Height;
    UINT32 Pitch;
    INT32  XHot;
    INT32  YHot;
    UINT32 SdrWhiteLevelX1000;
    UINT32 ShapeBufferSize;
    UINT32 Reserved0;
    UINT64 LastUpdateQpc;
    // Followed by up to 256 KiB of shape pixels.
  };

  static constexpr UINT32 VDD_CURSOR_MAGIC = 0x5A564355;  // 'ZVCU'
  static constexpr UINT32 VDD_CURSOR_VERSION = 1;
  static constexpr UINT32 VDD_CURSOR_MAX_BYTES = 256u * 256u * 4u;  // matches driver

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
    if (m_pCursorMeta) {
      UnmapViewOfFile(m_pCursorMeta);
      m_pCursorMeta = nullptr;
    }
    if (m_hCursorMeta) {
      CloseHandle(m_hCursorMeta);
      m_hCursorMeta = nullptr;
    }
    if (m_hCursorEvent) {
      CloseHandle(m_hCursorEvent);
      m_hCursorEvent = nullptr;
    }
    m_lastSeenCursorShapeId = 0xFFFFFFFFu;
    m_lastSeenCursorPositionId = 0xFFFFFFFFu;
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

    // Optional: attach to the cursor SHM exported by the driver-side
    // CursorExporter. Best-effort -- old driver builds won't have these
    // mappings, in which case poll_cursor() returns false and we render
    // frames without an overlay cursor (preserving previous behaviour).
    {
      std::wstring cursor_meta_name = L"Global\\ZakoVDD_CursorMeta_" + std::to_wstring(monitor_idx);
      std::wstring cursor_event_name = L"Global\\ZakoVDD_CursorReady_" + std::to_wstring(monitor_idx);

      m_hCursorMeta = OpenFileMappingW(FILE_MAP_READ, FALSE, cursor_meta_name.c_str());
      if (m_hCursorMeta) {
        const SIZE_T map_size = sizeof(CursorSharedMetadata) + VDD_CURSOR_MAX_BYTES;
        m_pCursorMeta = MapViewOfFile(m_hCursorMeta, FILE_MAP_READ, 0, 0, map_size);
        if (!m_pCursorMeta) {
          BOOST_LOG(warning) << "[vdd_capture] cursor MapViewOfFile failed: "sv << GetLastError()
                             << "; cursor overlay disabled."sv;
          CloseHandle(m_hCursorMeta);
          m_hCursorMeta = nullptr;
        }
        else {
          // Event is purely diagnostic / future poll-driven wait; poll_cursor()
          // works directly off the mapping so absence is non-fatal.
          m_hCursorEvent = OpenEventW(SYNCHRONIZE, FALSE, cursor_event_name.c_str());
          BOOST_LOG(info) << "[vdd_capture] cursor SHM attached (event="sv
                          << (m_hCursorEvent ? "yes"sv : "no"sv) << ")"sv;
        }
      }
      else {
        BOOST_LOG(info) << "[vdd_capture] cursor SHM not present for monitor "sv
                        << monitor_idx << " (driver may predate cursor export); "sv
                        << "clients will see no overlay cursor for this output."sv;
      }
    }

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

  bool
  vdd_capture_t::poll_cursor(cursor_snapshot &out) {
    if (!m_pCursorMeta) {
      return false;
    }

    auto *meta = static_cast<const CursorSharedMetadata *>(m_pCursorMeta);

    // Lock-free read pattern: snapshot header, then verify ShapeBufferSize
    // sanity. The producer writes payload before flipping ShapeId; if we read
    // a torn shape we'll simply pick it up on the next poll (cursor state
    // remains valid otherwise).
    UINT32 magic = meta->Magic;
    UINT32 version = meta->Version;
    if (magic != VDD_CURSOR_MAGIC || version != VDD_CURSOR_VERSION) {
      return false;  // producer hasn't published yet, or version skew
    }

    UINT32 shape_id = meta->ShapeId;
    UINT32 position_id = meta->PositionId;
    UINT32 shape_buffer_size = meta->ShapeBufferSize;
    if (shape_buffer_size > VDD_CURSOR_MAX_BYTES) {
      return false;  // torn read; try again next time
    }

    // Snapshot geometry headers and validate them before publishing. A torn
    // read of the header could leave Width/Height/Pitch/ShapeType inconsistent
    // with ShapeBufferSize; downstream make_cursor_alpha_image() /
    // make_cursor_xor_image() do pointer arithmetic against
    // Pitch * Height into out.shape_buffer, so any mismatch must be rejected
    // here to avoid out-of-bounds reads.
    UINT32 shape_type = meta->ShapeType;
    UINT32 width = meta->Width;
    UINT32 height = meta->Height;
    UINT32 pitch = meta->Pitch;
    if (shape_type > 2) {
      return false;  // unknown shape type
    }
    // Hardware cursor sizes are capped at 256x256 (512 tall for monochrome
    // which stores AND+XOR masks back-to-back). Reject anything bigger so a
    // bogus header can't get past the size budget check below via overflow.
    constexpr UINT32 kMaxCursorDim = 512;
    if (width == 0 || width > kMaxCursorDim || height == 0 || height > kMaxCursorDim) {
      return false;
    }
    // Per-type minimum pitch sanity. For color/masked-color each row is RGBA
    // (>= 4*width). For monochrome each row is one packed-bit AND/XOR mask
    // (>= ceil(width/8)). Height for monochrome encodes AND+XOR stacked, so
    // it must be even.
    UINT64 required_bytes = 0;
    if (shape_type == 0 /* MONOCHROME */) {
      if ((height & 1u) != 0u || pitch < (width + 7u) / 8u) {
        return false;
      }
      required_bytes = static_cast<UINT64>(pitch) * height;  // AND+XOR stacked
    } else {
      if (pitch < static_cast<UINT64>(width) * 4u) {
        return false;
      }
      required_bytes = static_cast<UINT64>(pitch) * height;
    }
    if (required_bytes > VDD_CURSOR_MAX_BYTES || shape_buffer_size < required_bytes) {
      return false;  // header/payload mismatch — likely torn read
    }

    out.valid = true;
    out.visible = (meta->IsVisible != 0);
    out.x = meta->PositionX;
    out.y = meta->PositionY;
    out.position_id = position_id;
    out.shape_id = shape_id;
    out.shape_type = shape_type;
    out.width = width;
    out.height = height;
    out.pitch = pitch;
    out.xhot = meta->XHot;
    out.yhot = meta->YHot;
    out.position_updated = (position_id != m_lastSeenCursorPositionId);
    out.shape_updated = (shape_id != m_lastSeenCursorShapeId);

    if (out.shape_updated && shape_buffer_size > 0) {
      const auto *payload = reinterpret_cast<const uint8_t *>(meta + 1);
      out.shape_buffer.assign(payload, payload + shape_buffer_size);
      // Re-check shape id after copy. If it changed mid-copy, drop this shape
      // and let the next poll pick up the consistent version.
      if (meta->ShapeId != shape_id) {
        out.shape_buffer.clear();
        out.shape_updated = false;
      }
    }

    if (out.shape_updated) m_lastSeenCursorShapeId = shape_id;
    if (out.position_updated) m_lastSeenCursorPositionId = position_id;
    return true;
  }

  // ===========================================================================
  // display_vdd_vram_t
  // ===========================================================================
  //
  // Resolves the VDD monitor index for a given DXGI display by probing
  // Global\ZakoVDD_Meta_<i> mappings and matching producer-side dimensions
  // against the DXGI output's reported size. Then opens the shared texture
  // on the same D3D11 device as display_base_t to avoid cross-device copies.

  // Probes Global\ZakoVDD_Meta_<i> for valid producers and returns:
  //   1) the index whose Width/Height exactly match target, or
  //   2) if no exact match exists but exactly one valid producer is present,
  //      that single producer's index (lets us start streaming and rely on
  //      vdd_capture_t::next_frame() to issue capture_e::reinit once the
  //      producer publishes new dimensions matching the requested mode), or
  //   3) -1 when no producer is reachable.
  static int
  resolve_vdd_monitor_index(unsigned int target_w, unsigned int target_h, unsigned int max_probe = 16) {
    int exact = -1;
    int only_valid = -1;
    int valid_count = 0;
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
      bool valid = (meta->Magic == VDD_META_MAGIC);
      unsigned mw = valid ? meta->Width : 0;
      unsigned mh = valid ? meta->Height : 0;
      unsigned mfmt = valid ? meta->DxgiFormat : 0;
      bool mhdr = valid && (meta->IsHdr != 0);
      UnmapViewOfFile(p);
      CloseHandle(h);
      if (!valid) continue;
      BOOST_LOG(info) << "[vdd] probe meta_"sv << i
                      << ": "sv << mw << "x"sv << mh
                      << " fmt="sv << mfmt << " hdr="sv << mhdr;
      ++valid_count;
      only_valid = static_cast<int>(i);
      if (exact < 0 && mw == target_w && mh == target_h) {
        exact = static_cast<int>(i);
      }
    }
    if (exact >= 0) {
      BOOST_LOG(info) << "[vdd] resolved monitor index "sv << exact
                      << " for "sv << target_w << "x"sv << target_h << " (exact match)"sv;
      return exact;
    }
    if (valid_count == 1 && only_valid >= 0) {
      BOOST_LOG(info) << "[vdd] no exact match for "sv << target_w << "x"sv << target_h
                      << "; falling back to sole producer monitor "sv << only_valid
                      << " (will reinit when producer publishes target mode)"sv;
      return only_valid;
    }
    if (valid_count == 0) {
      BOOST_LOG(warning) << "[vdd] no valid VDD producer found (no Meta_* mappings). "sv
                         << "Is the ZakoVDD driver installed and running?"sv;
    } else {
      BOOST_LOG(warning) << "[vdd] "sv << valid_count
                         << " VDD producers present but none match "sv
                         << target_w << "x"sv << target_h
                         << " and ambiguity prevents fallback."sv;
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
    // NOTE: We intentionally do NOT trigger CREATEMONITOR here. Sunshine's
    // display-device layer (prepare_vdd) already manages monitor lifecycle, and
    // adding a 3s NamedPipe round-trip per encoder probe wastes ~20s on every
    // startup with no real benefit. If no producer is reachable, we just fail
    // and let the upper layer try a different backend.
    int idx = resolve_vdd_monitor_index(static_cast<unsigned>(width_before_rotation),
                                        static_cast<unsigned>(height_before_rotation));
    if (idx < 0) {
      return -1;
    }
    monitor_idx = static_cast<unsigned int>(idx);

    if (dup.init(device.get(), monitor_idx) != 0) {
      BOOST_LOG(error) << "[vdd] vdd_capture_t::init failed for monitor "sv << monitor_idx;
      return -1;
    }

    // Use producer-reported format as our capture format. complete_img() / image
    // pool will be created against this format.
    capture_format = dup.format();
    capture_linear_gamma = capture_format == DXGI_FORMAT_R16G16B16A16_FLOAT;

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
                    << " hdr="sv << dup.is_hdr()
                    << " linear_gamma="sv << capture_linear_gamma;

    // Initialise the shared cursor blend pipeline (same shaders / blend states
    // as display_ddup_vram_t). If init fails we leave it for the caller to
    // tear down -- there is no degraded mode that's worth supporting because
    // the same shaders are required for HDR output anyway.
    if (init_cursor_pipeline(config) != 0) {
      BOOST_LOG(error) << "[vdd] cursor pipeline init failed"sv;
      return -1;
    }

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
    auto finite_clamped = [](float value, float min_value, float max_value) {
      if (!std::isfinite(value)) {
        return min_value;
      }
      return std::clamp(value, min_value, max_value);
    };

    metadata.maxDisplayLuminance = static_cast<uint16_t>(finite_clamped(dup.max_nits(), 0.0f, 65535.0f));
    metadata.minDisplayLuminance = static_cast<uint32_t>(finite_clamped(dup.min_nits(), 0.0f, 429496.7295f) * 10000.0f);
    metadata.maxFullFrameLuminance = static_cast<uint16_t>(finite_clamped(dup.max_fall(), 0.0f, 65535.0f));

    // Producer doesn't currently track per-frame content light levels.
    metadata.maxContentLightLevel = 0;
    metadata.maxFrameAverageLightLevel = 0;
    return true;
  }

  // NOTE: snapshot() and release_snapshot() are implemented in display_vram.cpp,
  // alongside display_amd_vram_t / display_wgc_vram_t, because they need access
  // to the file-local types `img_d3d_t` and `texture_lock_helper`.

}  // namespace platf::dxgi
