/**
 * @file src/platform/windows/mic_write.h
 * @brief Declarations for Windows microphone write functionality.
 */
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "src/platform/common.h"

// Windows includes
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

namespace platf::audio {

  struct mic_redirect_test_result_t {
    bool success = false;
    std::string error_code;
    std::string backend;
  };

  struct mic_redirect_status_t {
    std::string configured_backend;
    std::string active_backend;
    std::string fallback_reason;
    bool component_available = false;
    bool online = false;
    bool device_created = false;
    bool host_streaming = false;
    std::uint32_t generation = 0;
    std::uint32_t buffered_bytes = 0;
    std::uint32_t underruns = 0;
    std::uint32_t dropped_frames = 0;
    std::uint32_t submit_errors = 0;
    std::int32_t last_error = 0;
    std::string state = "absent";
    std::string error_code;
  };

  /**
   * @brief Write a short test tone through the configured virtual microphone.
   */
  mic_redirect_test_result_t
  test_mic_redirect();

  /** Runtime state consumed by the authenticated Web UI status endpoint. */
  mic_redirect_status_t
  mic_redirect_status();

  /** Update the selected backend after initialization or fallback. */
  void
  report_mic_redirect_backend(std::string active_backend, std::string fallback_reason = {});

  /** Atomically reserve the redirect path for a UI test. */
  bool
  try_begin_mic_redirect_test();

  /** Release a redirect-path reservation obtained by try_begin_mic_redirect_test(). */
  void
  end_mic_redirect_test();
  
  // COM interface Release helper for safe_ptr
  template<typename T>
  inline void Release(T *p) {
    if (p) p->Release();
  }
  
  // COM interface smart pointer types
  using device_enum_t = util::safe_ptr<IMMDeviceEnumerator, Release<IMMDeviceEnumerator>>;
  using audio_client_t = util::safe_ptr<IAudioClient, Release<IAudioClient>>;

  // Forward declarations for types used in mic_write_wasapi_t
  enum class match_field_e {
    device_id,  ///< Match device_id
    device_friendly_name,  ///< Match endpoint friendly name
    adapter_friendly_name,  ///< Match adapter friendly name
    device_description,  ///< Match endpoint description
  };
  using match_fields_list_t = std::vector<std::pair<match_field_e, std::wstring>>;
  using matched_field_t = std::pair<match_field_e, std::wstring>;

  /**
   * @brief Windows WASAPI microphone write class for client mic redirection
   * 
   * This class handles writing mixed client microphone PCM to virtual audio devices.
   */
  class mic_write_wasapi_t: public mic_t {
  public:
    mic_write_wasapi_t() = default;
    ~mic_write_wasapi_t() override;

    std::atomic<bool> is_cleaning_up = false;

    // This class is not for sampling, only for writing
    capture_e
    sample(std::vector<float> &sample_out) override;

    /**
     * @brief Initialize the microphone write device
     * @return 0 on success, -1 on failure
     */
    int
    init(bool test_mode = false);

    /**
     * @brief Write mono 48 kHz signed 16-bit PCM to the virtual audio device.
     * @param samples Pointer to the PCM samples.
     * @param frame_count Number of mono frames to write.
     * @return Number of bytes written, -1 on a generic error, or -2 when the device was invalidated.
     */
    int
    write_pcm(const std::int16_t *samples, std::size_t frame_count);

    /**
     * @brief Write a short audible tone to the initialized render endpoint.
     * @return Number of bytes written, or -1 on error
     */
    int
    test_write();

    /**
     * @brief Restore audio devices to their original state
     * @return 0 on success, -1 on error
     */
    int
    restore_audio_devices();

    /**
     * @brief Cleanup and release resources
     */
    void
    cleanup();

  private:
    // Virtual device type enumeration
    enum class VirtualDeviceType {
      NONE,
      STEAM,
      VB_CABLE,
    };

    /**
     * @brief Create or use virtual audio device
     * @return 0 on success, -1 on failure
     */
    int
    create_virtual_audio_device();

    /**
     * @brief Setup virtual microphone loopback
     * @return 0 on success, -1 on failure
     */
    int
    setup_virtual_mic_loopback();

    /**
     * @brief Setup Steam virtual microphone loopback
     * @return 0 on success, -1 on failure
     */
    int
    setup_steam_mic_loopback();

    /**
     * @brief Setup VB-Cable virtual microphone loopback
     * @return 0 on success, -1 on failure
     */
    int
    setup_vb_cable_mic_loopback();

    /**
     * @brief Find device ID by matching criteria
     * @param match_list List of match criteria
     * @return Optional matched field if found
     */
    std::optional<matched_field_t>
    find_device_id(const match_fields_list_t &match_list);

    /**
     * @brief Find capture device ID by matching criteria
     * @param match_list List of match criteria
     * @return Optional matched field if found
     */
    std::optional<matched_field_t>
    find_capture_device_id(const match_fields_list_t &match_list);

    /**
     * @brief Find device in collection by matching criteria
     * @param collection Device collection to search
     * @param match_list List of match criteria
     * @return Optional matched field if found
     */
    std::optional<matched_field_t>
    find_device_in_collection(void *collection, const match_fields_list_t &match_list);

    /**
     * @brief Set default device for all roles
     * @param device_id Device ID to set as default
     */
    HRESULT
    set_default_device_all_roles(const std::wstring &device_id);

    /**
     * @brief Store original audio device settings for restoration
     */
    void
    store_original_audio_settings();

    /**
     * @brief Restore original default audio output device
     * @return 0 on success, -1 on error
     */
    int
    restore_original_output_device();

    /**
     * @brief Restore original default audio input device
     * @return 0 on success, -1 on error
     */
    int
    restore_original_input_device();

    // Member variables
    device_enum_t device_enum;
    audio_client_t audio_client;
    IAudioRenderClient *audio_render = nullptr;
    HANDLE mmcss_task_handle = nullptr;
    WAVEFORMATEX current_format = {};
    VirtualDeviceType virtual_device_type = VirtualDeviceType::NONE;

    // Audio device restoration state
    struct {
      std::wstring original_input_device_id;
      bool input_device_changed = false;
      bool settings_stored = false;
    } restoration_state;
  };

  extern std::unique_ptr<mic_write_wasapi_t> mic_redirect_device;
}  // namespace platf::audio
