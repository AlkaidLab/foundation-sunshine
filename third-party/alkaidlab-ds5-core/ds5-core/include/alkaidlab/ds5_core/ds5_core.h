#ifndef ALKAIDLAB_DS5_CORE_DS5_CORE_H
#define ALKAIDLAB_DS5_CORE_DS5_CORE_H

/*
 * Standalone DualSense semantic core.  This header intentionally includes no
 * Platform, Runtime, SDK, or operating-system header.  The layout and append
 * only table rules are part of the Stage 0 ABI freeze.  Sources: ds5-linux-
 * hid-playstation-20261031 sections input/output reports and ds5-dualsense-
 * report-20261031 report lengths; ds5-sdl-hidapi-ps5-20261031 is the
 * independent implementation cross-check. Field-by-field offsets are in
 * REPORT_LAYOUTS.md. The latter is a secondary reference and unknown fields
 * remain fail-closed.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALK_DS5_CORE_ABI_VERSION 1u
#define ALK_DS5_CORE_MAX_DEVICES 2u
#define ALK_DS5_USB_INPUT_REPORT_BYTES 64u
#define ALK_DS5_USB_OUTPUT_REPORT_BYTES 63u
#define ALK_DS5_BT_INPUT_REPORT_BYTES 78u
#define ALK_DS5_BT_OUTPUT_REPORT_BYTES 78u
#define ALK_DS5_OUTPUT_COMMON_BYTES 47u
#define ALK_DS5_OUTPUT_VALID_FLAG0_OFFSET 1u
#define ALK_DS5_OUTPUT_VALID_FLAG1_OFFSET 2u
#define ALK_DS5_OUTPUT_VALID_FLAG2_OFFSET 39u
#define ALK_DS5_INPUT_BUTTONS0_OFFSET 8u
#define ALK_DS5_INPUT_BUTTONS1_OFFSET 9u
#define ALK_DS5_INPUT_BUTTONS2_OFFSET 10u
#define ALK_DS5_INPUT_BUTTONS3_OFFSET 11u
#define ALK_DS5_FEATURE_CALIBRATION_ID 0x05u
#define ALK_DS5_FEATURE_PAIRING_ID 0x09u
#define ALK_DS5_FEATURE_FIRMWARE_ID 0x20u
#define ALK_DS5_FEATURE_CALIBRATION_BYTES 41u
#define ALK_DS5_FEATURE_PAIRING_BYTES 20u
#define ALK_DS5_FEATURE_FIRMWARE_BYTES 64u

typedef enum AlkDs5Error {
  ALK_DS5_OK = 0,
  ALK_DS5_ERROR_INVALID_ARGUMENT = 1,
  ALK_DS5_ERROR_BUFFER_TOO_SMALL = 2,
  ALK_DS5_ERROR_MALFORMED_REPORT = 3,
  ALK_DS5_ERROR_UNSUPPORTED_REPORT = 4,
  ALK_DS5_ERROR_CRC_MISMATCH = 5,
  ALK_DS5_ERROR_STATE_ORDER = 6,
  ALK_DS5_ERROR_CAPABILITY_UNAVAILABLE = 7,
  ALK_DS5_ERROR_IDENTITY_CONFLICT = 8,
  ALK_DS5_ERROR_QUEUE_FULL = 9,
  ALK_DS5_ERROR_NOT_FOUND = 10,
  ALK_DS5_ERROR_RESOURCE = 11,
  ALK_DS5_ERROR_BAD_CRC = 12,
  ALK_DS5_ERROR_SESSION = 13,
} AlkDs5Error;

typedef enum AlkDs5Transport {
  ALK_DS5_TRANSPORT_USB = 1,
  ALK_DS5_TRANSPORT_BLUETOOTH = 2,
} AlkDs5Transport;

enum {
  ALK_DS5_CAP_BUTTONS = 1u << 0,
  ALK_DS5_CAP_STICKS = 1u << 1,
  ALK_DS5_CAP_TOUCH = 1u << 2,
  ALK_DS5_CAP_MOTION = 1u << 3,
  ALK_DS5_CAP_BATTERY = 1u << 4,
  ALK_DS5_CAP_RUMBLE = 1u << 5,
  ALK_DS5_CAP_ADAPTIVE_TRIGGERS = 1u << 6,
  ALK_DS5_CAP_LIGHTBAR = 1u << 7,
  ALK_DS5_CAP_PLAYER_LIGHT = 1u << 8,
  ALK_DS5_CAP_FEATURE_REPORTS = 1u << 9,
  ALK_DS5_CAP_AUDIO_RENDER = 1u << 10,
  ALK_DS5_CAP_AUDIO_CAPTURE = 1u << 11,
  ALK_DS5_CAP_MICROPHONE = 1u << 12,
  ALK_DS5_CAP_SPEAKER = 1u << 13,
  ALK_DS5_CAP_HEADSET_JACK = 1u << 14,
  ALK_DS5_CAP_MIC_MUTE = 1u << 15,
  ALK_DS5_CAP_AUDIO_PCM = 1u << 16,
  ALK_DS5_CAP_AUDIO_IR = 1u << 17,
  ALK_DS5_CAP_BLUETOOTH = 1u << 18,
  ALK_DS5_CAP_USBIP_PROVIDER = 1u << 19,
  ALK_DS5_CAP_USBIP_ISO = 1u << 20,
  ALK_DS5_CAP_SESSION_LEASE = 1u << 21,
  ALK_DS5_CAP_EDGE_PADDLES = 1u << 22,
  ALK_DS5_CAP_EDGE_PROFILES = 1u << 23,
  ALK_DS5_CAP_EDGE_TRIGGER_STOPS = 1u << 24,
  ALK_DS5_CAP_HEADSET_AUDIO = 1u << 25,
  ALK_DS5_CAP_AUDIO_ROUTING = 1u << 26,
  ALK_DS5_CAP_HD_HAPTICS = 1u << 27,
};

enum {
  ALK_DS5_FEATURE_CALIBRATION = 1u << 0,
  ALK_DS5_FEATURE_PAIRING = 1u << 1,
  ALK_DS5_FEATURE_FIRMWARE = 1u << 2,
};

typedef struct AlkDs5BackendEvidence AlkDs5BackendEvidence;

typedef struct AlkDs5Profile {
  uint32_t struct_size;
  uint32_t abi;
  uint16_t vendor_id;
  uint16_t product_id;
  uint32_t transport;
  uint32_t max_devices;
  const char *product_string;
  const char *manufacturer;
  const char *serial;
  const AlkDs5BackendEvidence *backend_evidence;
} AlkDs5Profile;

typedef struct AlkDs5AudioFacts {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t sample_rate_hz;
  uint16_t bits_per_sample;
  uint8_t channel_count;
  uint8_t channel_roles[8];
  uint32_t max_queue_frames;
  uint32_t endpoint_binds_to_device;
  uint32_t supports_render;
  uint32_t supports_capture;
} AlkDs5AudioFacts;

typedef struct AlkDs5EdgeFacts {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t paddles;
  uint32_t function_buttons;
  uint32_t profiles;
  uint32_t trigger_stops;
} AlkDs5EdgeFacts;

typedef struct AlkDs5DeviceFacts {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t capabilities;
  uint32_t feature_reports;
  uint32_t input_report_bytes;
  uint32_t output_report_bytes;
  uint32_t report_rate_hz;
  uint32_t game_recognition;
  uint32_t requires_signed_transport;
  uint32_t failure_scope;
  AlkDs5AudioFacts audio;
  uint16_t vendor_id;
  uint16_t product_id;
  uint32_t transport;
  uint32_t evidence_flags;
  uint8_t stable_mac[6];
  uint8_t reserved_identity[2];
  AlkDs5EdgeFacts edge;
  uint8_t stable_macs[ALK_DS5_CORE_MAX_DEVICES][6];
} AlkDs5DeviceFacts;

typedef struct AlkDs5BackendEvidence {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t proven_capabilities;
  uint32_t feature_reports;
  uint32_t transport_mask;
  uint32_t flags;
  uint32_t descriptor_config_bytes;
  uint32_t descriptor_hid_report_bytes;
  uint32_t iso_out_packet_bytes;
  uint32_t iso_in_packet_bytes;
  uint32_t iso_interval;
  AlkDs5AudioFacts audio;
  uint8_t stable_mac[6];
  uint8_t reserved[2];
  AlkDs5EdgeFacts edge;
} AlkDs5BackendEvidence;

typedef struct AlkDs5State {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t device_index;
  uint64_t sequence;
  uint64_t buttons;
  uint8_t hat;
  uint8_t left_trigger;
  uint8_t right_trigger;
  uint8_t reserved;
  uint8_t left_x;
  uint8_t left_y;
  uint8_t right_x;
  uint8_t right_y;
  uint8_t buttons3;
} AlkDs5State;

typedef struct AlkDs5BatteryState {
  uint32_t struct_size;
  uint32_t abi;
  uint8_t level_steps;
  uint8_t charging_state;
  uint8_t full;
  uint8_t reserved;
} AlkDs5BatteryState;

typedef struct AlkDs5JackState {
  uint32_t struct_size;
  uint32_t abi;
  uint8_t headset_present;
  uint8_t microphone_present;
  uint8_t microphone_muted;
  uint8_t reserved;
} AlkDs5JackState;

typedef struct AlkDs5Touch {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t device_index;
  uint64_t sequence;
  uint8_t contact_id;
  uint8_t active;
  uint16_t x;
  uint16_t y;
  uint16_t pressure;
} AlkDs5Touch;

typedef struct AlkDs5Motion {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t device_index;
  uint64_t sequence;
  int16_t gyro[3];
  int16_t accelerometer[3];
  uint32_t sensor_timestamp;
} AlkDs5Motion;

typedef struct AlkDs5ExtendedInput {
  uint32_t struct_size;
  uint32_t abi;
  AlkDs5State state;
  AlkDs5Touch touch[2];
  AlkDs5Motion motion;
  AlkDs5BatteryState battery;
  AlkDs5JackState jack;
} AlkDs5ExtendedInput;

typedef enum AlkDs5EventKind {
  ALK_DS5_EVENT_ARRIVAL = 1,
  ALK_DS5_EVENT_STATE = 2,
  ALK_DS5_EVENT_REMOVE = 3,
} AlkDs5EventKind;

typedef struct AlkDs5Event {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t device_index;
  uint32_t kind;
  uint64_t sequence;
  AlkDs5State state;
} AlkDs5Event;

typedef struct AlkDs5CapabilityLoss {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t requested;
  uint32_t provided;
  uint32_t lost;
  uint32_t error_class;
  const char *reason;
} AlkDs5CapabilityLoss;

typedef enum AlkDs5TriggerMode {
  ALK_DS5_TRIGGER_OFF = 0,
  ALK_DS5_TRIGGER_FEEDBACK = 1,
  ALK_DS5_TRIGGER_WEAPON = 2,
  ALK_DS5_TRIGGER_VIBRATION = 3,
  ALK_DS5_TRIGGER_SLOPE = 4,
  ALK_DS5_TRIGGER_RAW = 5,
} AlkDs5TriggerMode;

typedef struct AlkDs5TriggerEffect {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t mode;
  uint32_t flags;
  uint8_t start;
  uint8_t end;
  uint8_t strength;
  uint8_t frequency;
  uint8_t raw[11];
} AlkDs5TriggerEffect;

typedef struct AlkDs5Feedback {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t device_index;
  uint32_t valid_mask;
  uint16_t low_frequency;
  uint16_t high_frequency;
  uint8_t lightbar_rgb[3];
  uint8_t lightbar_brightness;
  uint8_t player_led_mask;
  uint8_t mute_led;
  uint8_t speaker_volume;
  uint8_t headphone_volume;
  uint8_t mic_volume;
  uint8_t audio_control;
  uint8_t valid_flag0;
  uint8_t valid_flag1;
  uint8_t valid_flag2;
  uint8_t mic_mute;
  AlkDs5TriggerEffect left_trigger;
  AlkDs5TriggerEffect right_trigger;
  uint8_t motor_right;
  uint8_t motor_left;
  uint8_t audio_control2;
  uint8_t reserved_feedback;
} AlkDs5Feedback;

typedef struct AlkDs5IrHaptics {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t device_index;
  uint16_t left_intensity;
  uint16_t right_intensity;
  uint16_t left_sharpness;
  uint16_t right_sharpness;
  uint64_t pts_ns;
} AlkDs5IrHaptics;

typedef struct AlkDs5AudioFrame {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t device_index;
  uint32_t sample_rate_hz;
  uint16_t bits_per_sample;
  uint8_t channel_count;
  uint8_t reserved;
  uint32_t frame_count;
  uint64_t pts_ns;
  const int16_t *samples;
  int16_t *output_samples;
} AlkDs5AudioFrame;

typedef struct AlkDs5FeatureFacts {
  uint32_t struct_size;
  uint32_t abi;
  uint8_t report_id;
  uint8_t stable_mac[6];
  uint8_t reserved0;
  uint32_t hardware_version;
  uint32_t firmware_version;
  int16_t calibration_values[17];
} AlkDs5FeatureFacts;

typedef struct AlkDs5Core AlkDs5Core;

AlkDs5Error alk_ds5_core_create(const AlkDs5Profile *profile, AlkDs5Core **out_core);
void alk_ds5_core_destroy(AlkDs5Core *core);
AlkDs5Error alk_ds5_core_read_facts(const AlkDs5Core *core, AlkDs5DeviceFacts *facts);
AlkDs5Error alk_ds5_core_arrive(AlkDs5Core *core, uint32_t device_index, AlkDs5Event *event);
AlkDs5Error alk_ds5_core_remove(AlkDs5Core *core, uint32_t device_index, AlkDs5Event *event);
AlkDs5Error alk_ds5_core_submit_state(AlkDs5Core *core, const AlkDs5State *state, AlkDs5Event *event);
AlkDs5Error alk_ds5_core_poll_event(AlkDs5Core *core, AlkDs5Event *event);
AlkDs5Error alk_ds5_core_describe_loss(const AlkDs5Core *core, uint32_t requested, AlkDs5CapabilityLoss *loss);
AlkDs5Error alk_ds5_core_begin_session(AlkDs5Core *core, uint64_t session_id);
AlkDs5Error alk_ds5_core_clear(AlkDs5Core *core);
AlkDs5Error alk_ds5_core_end_session(AlkDs5Core *core, uint64_t session_id);
AlkDs5Error alk_ds5_core_audio_write(AlkDs5Core *core, const AlkDs5AudioFrame *frame);
AlkDs5Error alk_ds5_core_audio_read(AlkDs5Core *core, AlkDs5AudioFrame *frame);
AlkDs5Error alk_ds5_core_audio_flush(AlkDs5Core *core);
AlkDs5Error alk_ds5_core_submit_ir_haptics(AlkDs5Core *core, const AlkDs5IrHaptics *haptics);
AlkDs5Error alk_ds5_core_get_ir_haptics(const AlkDs5Core *core, uint32_t device_index, AlkDs5IrHaptics *haptics);
AlkDs5Error alk_ds5_core_set_feature_report(AlkDs5Core *core, uint8_t report_id, const uint8_t *report, size_t length);
AlkDs5Error alk_ds5_core_get_feature_report(const AlkDs5Core *core, uint8_t report_id, uint8_t *report, size_t capacity,
                                            size_t *length);
AlkDs5Error alk_ds5_core_set_feature_report_for_device(AlkDs5Core *core, uint32_t device_index, uint8_t report_id,
                                                       const uint8_t *report, size_t length);
AlkDs5Error alk_ds5_core_get_feature_report_for_device(const AlkDs5Core *core, uint32_t device_index, uint8_t report_id,
                                                       uint8_t *report, size_t capacity, size_t *length);
AlkDs5Error alk_ds5_core_set_stable_mac(AlkDs5Core *core, uint32_t device_index, const uint8_t mac[6]);
AlkDs5Error alk_ds5_core_get_stable_mac(const AlkDs5Core *core, uint32_t device_index, uint8_t mac[6]);

AlkDs5Error alk_ds5_parse_input_report(const uint8_t *report, size_t length, AlkDs5State *state);
AlkDs5Error alk_ds5_parse_input_report_ex(const uint8_t *report, size_t length, AlkDs5ExtendedInput *input);
AlkDs5Error alk_ds5_build_input_report(const AlkDs5State *state, uint8_t *report, size_t capacity, size_t *length);
AlkDs5Error alk_ds5_build_output_report(const AlkDs5Feedback *feedback, uint8_t *report, size_t capacity,
                                        size_t *length);
AlkDs5Error alk_ds5_wrap_bluetooth_output_report(const uint8_t *usb_report, size_t usb_length, uint8_t *report,
                                                 size_t capacity, size_t *length);
/* Appended operation: the Bluetooth output report carries a four-bit sequence
 * in the high nibble of byte 1 (Linux hid-playstation DS_OUTPUT_SEQ_NO,
 * source key ds5-linux-hid-playstation-20261031). Firmware advances it once per
 * report. sequence is truncated to its low four bits; the existing wrapper is
 * this function with sequence 0. */
AlkDs5Error alk_ds5_wrap_bluetooth_output_report_seq(const uint8_t *usb_report, size_t usb_length, uint8_t sequence,
                                                     uint8_t *report, size_t capacity, size_t *length);
AlkDs5Error alk_ds5_parse_output_report(const uint8_t *report, size_t length, uint32_t *valid_flags);
AlkDs5Error alk_ds5_parse_output_report_ex(const uint8_t *report, size_t length, AlkDs5Feedback *feedback);
AlkDs5Error alk_ds5_feature_report_length(uint8_t report_id, size_t *length);
AlkDs5Error alk_ds5_parse_feature_report(uint8_t report_id, const uint8_t *report, size_t length,
                                         AlkDs5FeatureFacts *facts);
AlkDs5Error alk_ds5_parse_bluetooth_input_report(const uint8_t *report, size_t length, AlkDs5ExtendedInput *input);
AlkDs5Error alk_ds5_parse_bluetooth_output_report(const uint8_t *report, size_t length, AlkDs5Feedback *feedback);
uint32_t alk_ds5_crc32(const uint8_t *data, size_t length);
uint32_t alk_ds5_crc32_seed(uint8_t seed, const uint8_t *data, size_t length);

/* Append-only high-level observation and feedback operations. These bridge
 * OS-facing providers to the frozen report/parser ABI without exposing OS
 * types or changing any existing layout. */
typedef struct AlkDs5TouchObservation {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t device_index;
  uint64_t sequence;
  uint8_t contact_id;
  uint8_t active;
  uint16_t x;
  uint16_t y;
  uint16_t pressure;
} AlkDs5TouchObservation;

typedef struct AlkDs5MotionObservation {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t device_index;
  uint64_t sequence;
  int16_t gyro[3];
  int16_t accelerometer[3];
  uint32_t sensor_timestamp;
} AlkDs5MotionObservation;

typedef struct AlkDs5BatteryObservation {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t device_index;
  uint64_t sequence;
  uint8_t level_steps;
  uint8_t charging_state;
  uint8_t full;
  uint8_t reserved;
} AlkDs5BatteryObservation;

typedef struct AlkDs5JackObservation {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t device_index;
  uint64_t sequence;
  uint8_t headset_present;
  uint8_t microphone_present;
  uint8_t microphone_muted;
  uint8_t reserved;
} AlkDs5JackObservation;

AlkDs5Error alk_ds5_core_submit_touch(AlkDs5Core *core, const AlkDs5TouchObservation *observation);
AlkDs5Error alk_ds5_core_submit_motion(AlkDs5Core *core, const AlkDs5MotionObservation *observation);
AlkDs5Error alk_ds5_core_submit_battery(AlkDs5Core *core, const AlkDs5BatteryObservation *observation);
AlkDs5Error alk_ds5_core_submit_jack(AlkDs5Core *core, const AlkDs5JackObservation *observation);
AlkDs5Error alk_ds5_core_submit_feedback(AlkDs5Core *core, const AlkDs5Feedback *feedback);

#ifdef __cplusplus
}
#endif

#endif
