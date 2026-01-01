/**
 * @file src/platform/windows/mic_write.h
 * @brief Declarations for Windows microphone write functionality.
 */
#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "src/platform/common.h"

// Windows includes
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

// Forward declarations
struct OpusDecoder;

namespace platf::audio {
  
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
   * This class handles writing client microphone data to virtual audio devices
   * for redirection purposes. It supports OPUS decoding and various audio formats.
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
    init();

    /**
     * @brief Write audio data to the virtual audio device
     * @param data Pointer to the audio data (OPUS encoded)
     * @param len Length of the audio data in bytes
     * @param seq Sequence number for FEC recovery (0 = unknown)
     * @return Number of bytes written, or -1 on error
     */
    int
    write_data(const char *data, size_t len, uint16_t seq = 0);

    /**
     * @brief Test write functionality with silent audio
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
    OpusDecoder *opus_decoder = nullptr;
    HANDLE mmcss_task_handle = nullptr;
    WAVEFORMATEX current_format = {};
    VirtualDeviceType virtual_device_type = VirtualDeviceType::NONE;

    // Audio device restoration state
    struct {
      std::wstring original_input_device_id;
      bool input_device_changed = false;
      bool settings_stored = false;
    } restoration_state;

    // FEC recovery state
    uint16_t last_seq = 0;
    bool first_packet = true;
    
    // Statistics
    uint64_t total_packets = 0;
    uint64_t packet_loss_count = 0;
    uint64_t fec_recovered_packets = 0;
  };

  extern std::unique_ptr<mic_write_wasapi_t> mic_redirect_device;
}  // namespace platf::audio 