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
#include "src/config.h"
#include "src/main.h"

#include <d3d11_1.h>
#include <sddl.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>
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
    UINT64 LastPublishQpc;
    UINT32 LastPresentationFrameNumber;
    UINT32 LastDirtyRectCount;
    UINT64 ReplacedUnreadFrames;
    UINT64 DroppedConsumerHeldFrames;
    UINT64 DroppedAcquireFailures;
    UINT32 MetadataSize;
    UINT32 SlotCount;
    UINT32 SlotIndex;
    UINT32 Reserved0;
    UINT32 AdapterLuidLowPart;
    INT32  AdapterLuidHighPart;
    UINT64 ProducerQpcFrequency;
  };

  static constexpr UINT32 VDD_META_MAGIC = 0x5A564446;  // 'ZVDF'
  static constexpr UINT32 VDD_META_VERSION = 1;
  static constexpr UINT32 VDD_MAX_SHARED_SLOTS = 8;

  static std::wstring
  vdd_texture_name(unsigned int monitor_idx, UINT32 slot_idx) {
    std::wstring base = L"Global\\ZakoVDD_Frame_" + std::to_wstring(monitor_idx);
    if (slot_idx == 0) {
      return base;
    }
    return base + L"_Slot_" + std::to_wstring(slot_idx);
  }

  static std::optional<bool>
  vdd_borrowed_texture_env_override() {
    const char *raw_value = std::getenv("SUNSHINE_VDD_BORROWED_TEXTURE");
    if (!raw_value || !*raw_value) {
      return std::nullopt;
    }

    std::string value { raw_value };
    std::transform(value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "1" || value == "true" || value == "on" || value == "yes" || value == "enabled") {
      return true;
    }
    if (value == "0" || value == "false" || value == "off" || value == "no" || value == "disabled") {
      return false;
    }
    BOOST_LOG(warning) << "[vdd] ignoring invalid SUNSHINE_VDD_BORROWED_TEXTURE value: "sv << value;
    return std::nullopt;
  }

  vdd_capture_t::vdd_capture_t() = default;

  vdd_capture_t::~vdd_capture_t() {
    close();
  }

  void
  vdd_capture_t::close() {
    if (m_holdsKey && m_heldSlot < m_keyedMutex.size() && m_keyedMutex[m_heldSlot]) {
      m_keyedMutex[m_heldSlot]->ReleaseSync(0);
      m_holdsKey = false;
    }
    m_keyedMutex.clear();
    m_sharedTex.clear();
    m_heldSlot = 0;
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
    m_lastPresentQpc = meta->LastPresentQpc;
    m_lastPublishQpc = meta->LastPublishQpc;
    m_lastPresentationFrameNumber = meta->LastPresentationFrameNumber;
    m_lastDirtyRectCount = meta->LastDirtyRectCount;
    m_replacedUnreadFrames = meta->ReplacedUnreadFrames;
    m_droppedConsumerHeldFrames = meta->DroppedConsumerHeldFrames;
    m_droppedAcquireFailures = meta->DroppedAcquireFailures;
    m_slotCount = meta->SlotCount ? std::min(meta->SlotCount, VDD_MAX_SHARED_SLOTS) : 1;
    m_slotIndex = meta->SlotIndex;
    m_producerQpcFrequency = meta->ProducerQpcFrequency;
    m_adapterLuid.LowPart = meta->AdapterLuidLowPart;
    m_adapterLuid.HighPart = meta->AdapterLuidHighPart;
    if (meta->SlotCount > VDD_MAX_SHARED_SLOTS) {
      BOOST_LOG(warning) << "[vdd_capture] producer advertised "sv << meta->SlotCount
                         << " slots; clamping to "sv << VDD_MAX_SHARED_SLOTS;
    }

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

    m_sharedTex.resize(m_slotCount);
    m_keyedMutex.resize(m_slotCount);
    for (UINT32 slot = 0; slot < m_slotCount; ++slot) {
      ID3D11Texture2D *raw = nullptr;
      auto tex_name = vdd_texture_name(monitor_idx, slot);
      hr = dev1->OpenSharedResourceByName(tex_name.c_str(),
                                          DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                          __uuidof(ID3D11Texture2D),
                                          reinterpret_cast<void **>(&raw));
      if (FAILED(hr) || !raw) {
        BOOST_LOG(error) << "[vdd_capture] OpenSharedResourceByName failed for slot "sv
                         << slot << ": 0x"sv << util::hex(hr).to_string_view()
                         << ". LUID mismatch with VDD RenderAdapter?"sv;
        close();
        return -1;
      }
      m_sharedTex[slot].reset(raw);

      IDXGIKeyedMutex *km = nullptr;
      hr = m_sharedTex[slot]->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(&km));
      if (FAILED(hr) || !km) {
        BOOST_LOG(error) << "[vdd_capture] no IDXGIKeyedMutex on shared texture slot "sv
                         << slot << ": 0x"sv << util::hex(hr).to_string_view();
        close();
        return -1;
      }
      m_keyedMutex[slot].reset(km);
    }

    BOOST_LOG(info) << "[vdd_capture] opened monitor "sv << monitor_idx
                    << " "sv << m_width << "x"sv << m_height
                    << " fmt="sv << static_cast<int>(m_format)
                    << " hdr="sv << m_is_hdr
                    << " luid="sv << m_adapterLuid.LowPart << ":"sv << m_adapterLuid.HighPart
                    << " slot="sv << m_slotIndex << "/"sv << m_slotCount
                    << " meta_size="sv << meta->MetadataSize;
    return 0;
  }

  capture_e
  vdd_capture_t::next_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out, uint64_t &out_frame_qpc) {
    if (out) *out = nullptr;
    out_frame_qpc = 0;

    if (!m_hEvent || m_keyedMutex.empty() || m_sharedTex.empty() || !m_pMeta) {
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

    // Producer released the latest slot as key 1; consumer acquires that slot.
    UINT32 slot = meta->SlotIndex;
    if (slot >= m_keyedMutex.size()) {
      BOOST_LOG(warning) << "[vdd_capture] producer slot "sv << slot
                         << " outside opened slot count "sv << m_keyedMutex.size()
                         << ", falling back to slot 0"sv;
      slot = 0;
    }

    HRESULT hr = m_keyedMutex[slot]->AcquireSync(1, ms);
    if (hr == static_cast<HRESULT>(WAIT_TIMEOUT)) {
      return capture_e::timeout;
    }
    if (FAILED(hr)) {
      BOOST_LOG(error) << "[vdd_capture] AcquireSync(1) failed for slot "sv << slot
                       << ": 0x"sv
                       << util::hex(hr).to_string_view();
      return capture_e::error;
    }
    m_holdsKey = true;
    m_heldSlot = slot;

    // Detect producer-side resize / format change: the metadata block can change
    // any time the swap chain is re-created. If so, signal reinit so the upper
    // layer reopens the shared texture.
    if (meta->Width != m_width || meta->Height != m_height ||
        static_cast<DXGI_FORMAT>(meta->DxgiFormat) != m_format) {
      BOOST_LOG(info) << "[vdd_capture] producer resolution/format changed, requesting reinit"sv;
      m_keyedMutex[m_heldSlot]->ReleaseSync(0);
      m_holdsKey = false;
      m_heldSlot = 0;
      return capture_e::reinit;
    }

    const auto previous_replaced = m_replacedUnreadFrames;
    const auto previous_held = m_droppedConsumerHeldFrames;
    const auto previous_acquire = m_droppedAcquireFailures;

    // Refresh producer metadata (cheap copy from shared mapping).
    m_is_hdr = (meta->IsHdr != 0);
    m_max_nits = meta->MaxNits;
    m_min_nits = meta->MinNits;
    m_max_fall = meta->MaxFALL;
    m_lastFrameCounter = meta->FrameCounter;
    m_lastPresentQpc = meta->LastPresentQpc;
    m_lastPublishQpc = meta->LastPublishQpc;
    m_lastPresentationFrameNumber = meta->LastPresentationFrameNumber;
    m_lastDirtyRectCount = meta->LastDirtyRectCount;
    m_replacedUnreadFrames = meta->ReplacedUnreadFrames;
    m_droppedConsumerHeldFrames = meta->DroppedConsumerHeldFrames;
    m_droppedAcquireFailures = meta->DroppedAcquireFailures;
    m_slotCount = meta->SlotCount ? std::min(meta->SlotCount, VDD_MAX_SHARED_SLOTS) : 1;
    m_slotIndex = slot;
    m_producerQpcFrequency = meta->ProducerQpcFrequency;
    m_adapterLuid.LowPart = meta->AdapterLuidLowPart;
    m_adapterLuid.HighPart = meta->AdapterLuidHighPart;
    out_frame_qpc = m_lastPresentQpc ? m_lastPresentQpc : m_lastPublishQpc;

    if (m_replacedUnreadFrames != previous_replaced ||
        m_droppedConsumerHeldFrames != previous_held ||
        m_droppedAcquireFailures != previous_acquire) {
      BOOST_LOG(debug) << "[vdd_capture] producer mailbox stats: frame="sv << m_lastFrameCounter
                       << " replaced_unread="sv << m_replacedUnreadFrames
                       << " dropped_consumer_held="sv << m_droppedConsumerHeldFrames
                       << " dropped_acquire_failures="sv << m_droppedAcquireFailures;
    }

    if (out) {
      m_sharedTex[slot]->AddRef();
      *out = m_sharedTex[slot].get();
    }
    return capture_e::ok;
  }

  capture_e
  vdd_capture_t::handoff_frame(UINT64 release_key, IDXGIKeyedMutex **out_mutex, UINT32 &out_slot) {
    if (out_mutex) *out_mutex = nullptr;
    out_slot = 0;

    if (!m_holdsKey || m_heldSlot >= m_keyedMutex.size() || !m_keyedMutex[m_heldSlot]) {
      BOOST_LOG(error) << "[vdd_capture] handoff_frame called without a held slot"sv;
      return capture_e::error;
    }

    auto *mutex = m_keyedMutex[m_heldSlot].get();
    HRESULT hr = mutex->ReleaseSync(release_key);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "[vdd_capture] ReleaseSync("sv << release_key
                       << ") failed during handoff for slot "sv << m_heldSlot
                       << ": 0x"sv << util::hex(hr).to_string_view();
      return capture_e::error;
    }

    out_slot = m_heldSlot;
    if (out_mutex) {
      mutex->AddRef();
      *out_mutex = mutex;
    }
    m_holdsKey = false;
    m_heldSlot = 0;
    return capture_e::ok;
  }

  capture_e
  vdd_capture_t::release_frame() {
    if (m_holdsKey && m_heldSlot < m_keyedMutex.size() && m_keyedMutex[m_heldSlot]) {
      m_keyedMutex[m_heldSlot]->ReleaseSync(0);
      m_holdsKey = false;
      m_heldSlot = 0;
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

  struct vdd_probe_result_t {
    int exact = -1;
    int only_valid = -1;
    int valid_count = 0;
    unsigned int only_valid_width = 0;
    unsigned int only_valid_height = 0;
  };

  static vdd_probe_result_t
  probe_vdd_monitor_index(unsigned int target_w, unsigned int target_h, unsigned int max_probe) {
    vdd_probe_result_t result;

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
      BOOST_LOG(debug) << "[vdd] probe meta_"sv << i
                       << ": "sv << mw << "x"sv << mh
                       << " fmt="sv << mfmt << " hdr="sv << mhdr;
      ++result.valid_count;
      result.only_valid = static_cast<int>(i);
      result.only_valid_width = mw;
      result.only_valid_height = mh;
      if (result.exact < 0 && mw == target_w && mh == target_h) {
        result.exact = static_cast<int>(i);
      }
    }

    return result;
  }

  // Probes Global\ZakoVDD_Meta_<i> for valid producers and returns the index
  // whose Width/Height exactly match target. A stale sole producer is not safe:
  // the encoder and capture surfaces are already sized for the requested mode,
  // so opening a mismatched producer can spin the stream in a reinit loop.
  static int
  resolve_vdd_monitor_index(unsigned int target_w, unsigned int target_h, unsigned int max_probe = 16) {
    constexpr auto retry_window = 2500ms;
    constexpr auto retry_delay = 100ms;

    vdd_probe_result_t last_result;
    const auto deadline = std::chrono::steady_clock::now() + retry_window;

    for (;;) {
      last_result = probe_vdd_monitor_index(target_w, target_h, max_probe);
      if (last_result.exact >= 0) {
        BOOST_LOG(info) << "[vdd] resolved monitor index "sv << last_result.exact
                        << " for "sv << target_w << "x"sv << target_h << " (exact match)"sv;
        return last_result.exact;
      }

      if (std::chrono::steady_clock::now() >= deadline) {
        break;
      }

      std::this_thread::sleep_for(retry_delay);
    }

    if (last_result.valid_count == 0) {
      BOOST_LOG(warning) << "[vdd] no valid VDD producer found (no Meta_* mappings). "sv
                         << "Is the ZakoVDD driver installed and running?"sv;
    }
    else if (last_result.valid_count == 1 && last_result.only_valid >= 0) {
      BOOST_LOG(warning) << "[vdd] sole VDD producer monitor "sv << last_result.only_valid
                         << " is "sv << last_result.only_valid_width
                         << "x"sv << last_result.only_valid_height
                         << ", but requested "sv << target_w << "x"sv << target_h
                         << "; refusing stale producer to avoid black screen/reinit loop."sv;
    }
    else {
      BOOST_LOG(warning) << "[vdd] "sv << last_result.valid_count
                         << " VDD producers present but none match "sv
                         << target_w << "x"sv << target_h
                         << "."sv;
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
    vdd_borrow_enabled = config::video.vdd_borrowed_texture;
    if (auto env_override = vdd_borrowed_texture_env_override()) {
      vdd_borrow_enabled = *env_override;
    }
    vdd_borrow_cooldown_until = {};
    vdd_borrow_last_telemetry = {};
    vdd_borrow_attempts = 0;
    vdd_borrow_successes = 0;
    vdd_borrow_fallbacks = 0;
    vdd_borrow_disabled_frames = 0;
    vdd_borrow_cooldown_frames = 0;
    vdd_borrow_cooldown_events = 0;
    vdd_borrow_deferred_frames = 0;
    vdd_borrow_returned_deferred_frames = 0;
    vdd_borrow_inflight_frames = std::make_shared<std::atomic<UINT64>>(0);
    vdd_borrow_inflight_limit_frames = 0;
    vdd_borrow_deferred_images.clear();
    vdd_last_replaced_unread = dup.replaced_unread_frames();
    vdd_last_dropped_consumer_held = dup.dropped_consumer_held_frames();
    vdd_last_dropped_acquire_failures = dup.dropped_acquire_failures();

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
                    << " linear_gamma="sv << capture_linear_gamma
                    << " borrowed_texture="sv << vdd_borrow_enabled;
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
