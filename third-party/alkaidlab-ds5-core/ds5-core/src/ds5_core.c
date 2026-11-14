#include "alkaidlab/ds5_core/ds5_core.h"

#include <stdlib.h>
#include <string.h>

/* USB report offsets follow the current hid-playstation DualSense layout;
 * see source key ds5-linux-hid-playstation-20261031. */
#define DS5_OFF_LEFT_X 1u
#define DS5_OFF_LEFT_Y 2u
#define DS5_OFF_RIGHT_X 3u
#define DS5_OFF_RIGHT_Y 4u
#define DS5_OFF_LEFT_TRIGGER 5u
#define DS5_OFF_RIGHT_TRIGGER 6u
#define DS5_OFF_BUTTONS0 ALK_DS5_INPUT_BUTTONS0_OFFSET
#define DS5_OFF_BUTTONS1 ALK_DS5_INPUT_BUTTONS1_OFFSET
#define DS5_OFF_BUTTONS2 ALK_DS5_INPUT_BUTTONS2_OFFSET
#define DS5_OFF_BUTTONS3 ALK_DS5_INPUT_BUTTONS3_OFFSET
#define DS5_AUDIO_QUEUE_FRAMES 2400u

struct AlkDs5Core {
  AlkDs5Profile profile;
  AlkDs5DeviceFacts facts;
  bool present[ALK_DS5_CORE_MAX_DEVICES];
  uint64_t sequence[ALK_DS5_CORE_MAX_DEVICES];
  AlkDs5State last_state[ALK_DS5_CORE_MAX_DEVICES];
  AlkDs5Event events[ALK_DS5_CORE_MAX_DEVICES * 2u + 2u];
  size_t event_head;
  size_t event_tail;
  uint64_t session_id;
  bool session_active;
  int16_t audio[ALK_DS5_CORE_MAX_DEVICES][DS5_AUDIO_QUEUE_FRAMES * 4u];
  uint32_t audio_read[ALK_DS5_CORE_MAX_DEVICES];
  uint32_t audio_write[ALK_DS5_CORE_MAX_DEVICES];
  uint32_t audio_frames[ALK_DS5_CORE_MAX_DEVICES];
  uint8_t feature_data[ALK_DS5_CORE_MAX_DEVICES][256][64];
  uint8_t feature_lengths[ALK_DS5_CORE_MAX_DEVICES][256];
  bool feature_present[ALK_DS5_CORE_MAX_DEVICES][256];
  uint8_t stable_macs[ALK_DS5_CORE_MAX_DEVICES][6];
  AlkDs5IrHaptics last_ir[ALK_DS5_CORE_MAX_DEVICES];
  bool ir_present[ALK_DS5_CORE_MAX_DEVICES];
  AlkDs5TouchObservation last_touch[ALK_DS5_CORE_MAX_DEVICES][2];
  AlkDs5MotionObservation last_motion[ALK_DS5_CORE_MAX_DEVICES];
  AlkDs5BatteryObservation last_battery[ALK_DS5_CORE_MAX_DEVICES];
  AlkDs5JackObservation last_jack[ALK_DS5_CORE_MAX_DEVICES];
  AlkDs5Feedback last_feedback[ALK_DS5_CORE_MAX_DEVICES];
  bool feedback_present[ALK_DS5_CORE_MAX_DEVICES];
};

static bool has_size(uint32_t actual, size_t needed) { return actual >= needed; }
static uint16_t read_le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8u); }
static uint32_t read_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static void init_facts(AlkDs5DeviceFacts *facts, const AlkDs5Profile *profile) {
  memset(facts, 0, sizeof(*facts));
  facts->struct_size = (uint32_t)sizeof(*facts);
  facts->abi = ALK_DS5_CORE_ABI_VERSION;
  facts->vendor_id = profile->vendor_id;
  facts->product_id = profile->product_id;
  facts->transport = profile->transport;
  const AlkDs5BackendEvidence *evidence = profile->backend_evidence;
  if (evidence != NULL && evidence->struct_size >= offsetof(AlkDs5BackendEvidence, abi) + sizeof(evidence->abi) &&
      evidence->abi == ALK_DS5_CORE_ABI_VERSION) {
    AlkDs5BackendEvidence evidence_copy;
    memset(&evidence_copy, 0, sizeof(evidence_copy));
    const size_t evidence_copy_size =
        evidence->struct_size < sizeof(evidence_copy) ? evidence->struct_size : sizeof(evidence_copy);
    memcpy(&evidence_copy, evidence, evidence_copy_size);
    facts->capabilities = evidence_copy.proven_capabilities;
    facts->feature_reports = evidence_copy.feature_reports;
    facts->evidence_flags = evidence_copy.flags;
    if (evidence->struct_size >= offsetof(AlkDs5BackendEvidence, stable_mac) + sizeof(evidence->stable_mac)) {
      memcpy(facts->stable_mac, evidence_copy.stable_mac, sizeof(facts->stable_mac));
    }
    if (evidence->struct_size >= offsetof(AlkDs5BackendEvidence, audio) + sizeof(evidence->audio)) {
      facts->audio = evidence_copy.audio;
    }
    if (evidence->struct_size >= offsetof(AlkDs5BackendEvidence, edge) + sizeof(evidence->edge)) {
      facts->edge = evidence_copy.edge;
    }
  }
  facts->input_report_bytes = profile->transport == ALK_DS5_TRANSPORT_BLUETOOTH ? ALK_DS5_BT_INPUT_REPORT_BYTES
                                                                                : ALK_DS5_USB_INPUT_REPORT_BYTES;
  facts->output_report_bytes = profile->transport == ALK_DS5_TRANSPORT_BLUETOOTH ? ALK_DS5_BT_OUTPUT_REPORT_BYTES
                                                                                 : ALK_DS5_USB_OUTPUT_REPORT_BYTES;
  facts->report_rate_hz = 1000u;
  facts->game_recognition = (facts->capabilities & ALK_DS5_CAP_BUTTONS) != 0u;
  facts->requires_signed_transport = 0u;
  facts->failure_scope = 1u;
  if (facts->audio.struct_size == 0u) {
    facts->audio.struct_size = (uint32_t)sizeof(facts->audio);
    facts->audio.abi = ALK_DS5_CORE_ABI_VERSION;
  }
}

static bool valid_device(const AlkDs5Core *core, uint32_t device_index) {
  return core != NULL && device_index < ALK_DS5_CORE_MAX_DEVICES && device_index < core->profile.max_devices;
}

static bool event_queue_full(const AlkDs5Core *core) {
  return core->event_tail - core->event_head >= sizeof(core->events) / sizeof(core->events[0]);
}

static AlkDs5Error enqueue(AlkDs5Core *core, const AlkDs5Event *event) {
  if (event_queue_full(core)) {
    return ALK_DS5_ERROR_QUEUE_FULL;
  }
  core->events[core->event_tail % (sizeof(core->events) / sizeof(core->events[0]))] = *event;
  ++core->event_tail;
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_create(const AlkDs5Profile *profile, AlkDs5Core **out_core) {
  if (profile == NULL || out_core == NULL || profile->abi != ALK_DS5_CORE_ABI_VERSION ||
      !has_size(profile->struct_size, offsetof(AlkDs5Profile, serial) + sizeof(profile->serial)) ||
      profile->vendor_id != 0x054cu || (profile->product_id != 0x0ce6u && profile->product_id != 0x0df2u) ||
      (profile->transport != ALK_DS5_TRANSPORT_USB && profile->transport != ALK_DS5_TRANSPORT_BLUETOOTH) ||
      profile->max_devices == 0u || profile->max_devices > ALK_DS5_CORE_MAX_DEVICES) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  AlkDs5Core *core = (AlkDs5Core *)calloc(1u, sizeof(*core));
  if (core == NULL) {
    return ALK_DS5_ERROR_RESOURCE;
  }
  memset(&core->profile, 0, sizeof(core->profile));
  const size_t profile_copy_size =
      profile->struct_size < sizeof(core->profile) ? profile->struct_size : sizeof(core->profile);
  memcpy(&core->profile, profile, profile_copy_size);
  init_facts(&core->facts, &core->profile);
  for (uint32_t index = 0u; index < ALK_DS5_CORE_MAX_DEVICES; ++index) {
    memcpy(core->stable_macs[index], core->facts.stable_mac, sizeof(core->stable_macs[index]));
  }
  memcpy(core->facts.stable_macs, core->stable_macs, sizeof(core->facts.stable_macs));
  *out_core = core;
  return ALK_DS5_OK;
}

void alk_ds5_core_destroy(AlkDs5Core *core) { free(core); }

AlkDs5Error alk_ds5_core_read_facts(const AlkDs5Core *core, AlkDs5DeviceFacts *facts) {
  if (core == NULL || facts == NULL || facts->abi != ALK_DS5_CORE_ABI_VERSION ||
      facts->struct_size < offsetof(AlkDs5DeviceFacts, capabilities) + sizeof(facts->capabilities)) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  const size_t copy_size = facts->struct_size < sizeof(core->facts) ? facts->struct_size : sizeof(core->facts);
  AlkDs5DeviceFacts facts_copy = core->facts;
  memcpy(facts_copy.stable_macs, core->stable_macs, sizeof(facts_copy.stable_macs));
  memcpy(facts_copy.stable_mac, core->stable_macs[0], sizeof(facts_copy.stable_mac));
  memcpy(facts, &facts_copy, copy_size);
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_arrive(AlkDs5Core *core, uint32_t device_index, AlkDs5Event *event) {
  if (!valid_device(core, device_index) || event == NULL || event->abi != ALK_DS5_CORE_ABI_VERSION ||
      event->struct_size < sizeof(*event)) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  if (core->present[device_index]) {
    return ALK_DS5_ERROR_IDENTITY_CONFLICT;
  }
  if (event_queue_full(core)) {
    return ALK_DS5_ERROR_QUEUE_FULL;
  }
  core->present[device_index] = true;
  ++core->sequence[device_index];
  memset(event, 0, sizeof(*event));
  event->struct_size = sizeof(*event);
  event->abi = ALK_DS5_CORE_ABI_VERSION;
  event->device_index = device_index;
  event->kind = ALK_DS5_EVENT_ARRIVAL;
  event->sequence = core->sequence[device_index];
  return enqueue(core, event);
}

AlkDs5Error alk_ds5_core_remove(AlkDs5Core *core, uint32_t device_index, AlkDs5Event *event) {
  if (!valid_device(core, device_index) || event == NULL || event->abi != ALK_DS5_CORE_ABI_VERSION ||
      event->struct_size < sizeof(*event)) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  if (!core->present[device_index]) {
    return ALK_DS5_ERROR_STATE_ORDER;
  }
  if (event_queue_full(core)) {
    return ALK_DS5_ERROR_QUEUE_FULL;
  }
  core->present[device_index] = false;
  ++core->sequence[device_index];
  memset(event, 0, sizeof(*event));
  event->struct_size = sizeof(*event);
  event->abi = ALK_DS5_CORE_ABI_VERSION;
  event->device_index = device_index;
  event->kind = ALK_DS5_EVENT_REMOVE;
  event->sequence = core->sequence[device_index];
  return enqueue(core, event);
}

AlkDs5Error alk_ds5_core_submit_state(AlkDs5Core *core, const AlkDs5State *state, AlkDs5Event *event) {
  if (!valid_device(core, state == NULL ? UINT32_MAX : state->device_index) || state == NULL || event == NULL ||
      state->abi != ALK_DS5_CORE_ABI_VERSION || state->struct_size < sizeof(*state) ||
      event->abi != ALK_DS5_CORE_ABI_VERSION || event->struct_size < sizeof(*event)) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  if (!core->present[state->device_index] || state->sequence <= core->sequence[state->device_index]) {
    return ALK_DS5_ERROR_STATE_ORDER;
  }
  if (event_queue_full(core)) {
    return ALK_DS5_ERROR_QUEUE_FULL;
  }
  core->sequence[state->device_index] = state->sequence;
  core->last_state[state->device_index] = *state;
  memset(event, 0, sizeof(*event));
  event->struct_size = sizeof(*event);
  event->abi = ALK_DS5_CORE_ABI_VERSION;
  event->device_index = state->device_index;
  event->kind = ALK_DS5_EVENT_STATE;
  event->sequence = state->sequence;
  event->state = *state;
  return enqueue(core, event);
}

AlkDs5Error alk_ds5_core_poll_event(AlkDs5Core *core, AlkDs5Event *event) {
  if (core == NULL || event == NULL || event->abi != ALK_DS5_CORE_ABI_VERSION || event->struct_size < sizeof(*event)) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  if (core->event_head == core->event_tail) {
    return ALK_DS5_ERROR_NOT_FOUND;
  }
  *event = core->events[core->event_head % (sizeof(core->events) / sizeof(core->events[0]))];
  ++core->event_head;
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_describe_loss(const AlkDs5Core *core, uint32_t requested, AlkDs5CapabilityLoss *loss) {
  if (core == NULL || loss == NULL || loss->abi != ALK_DS5_CORE_ABI_VERSION || loss->struct_size < sizeof(*loss)) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  loss->requested = requested;
  loss->provided = core->facts.capabilities;
  loss->lost = requested & ~loss->provided;
  loss->error_class = loss->lost == 0u ? ALK_DS5_OK : ALK_DS5_ERROR_CAPABILITY_UNAVAILABLE;
  loss->reason = loss->lost == 0u ? "capabilities available" : "backend did not prove requested capability";
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_begin_session(AlkDs5Core *core, uint64_t session_id) {
  if (core == NULL || session_id == 0u) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  if (core->session_active && core->session_id != session_id) {
    return ALK_DS5_ERROR_SESSION;
  }
  core->session_id = session_id;
  core->session_active = true;
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_clear(AlkDs5Core *core) {
  if (core == NULL) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  memset(core->present, 0, sizeof(core->present));
  memset(core->last_state, 0, sizeof(core->last_state));
  memset(core->audio_read, 0, sizeof(core->audio_read));
  memset(core->audio_write, 0, sizeof(core->audio_write));
  memset(core->audio_frames, 0, sizeof(core->audio_frames));
  memset(core->feature_present, 0, sizeof(core->feature_present));
  memset(core->last_ir, 0, sizeof(core->last_ir));
  memset(core->ir_present, 0, sizeof(core->ir_present));
  core->event_head = core->event_tail;
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_end_session(AlkDs5Core *core, uint64_t session_id) {
  if (core == NULL || !core->session_active || core->session_id != session_id) {
    return ALK_DS5_ERROR_SESSION;
  }
  alk_ds5_core_clear(core);
  core->session_active = false;
  core->session_id = 0u;
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_audio_write(AlkDs5Core *core, const AlkDs5AudioFrame *frame) {
  if (core == NULL || frame == NULL || frame->struct_size < offsetof(AlkDs5AudioFrame, abi) + sizeof(frame->abi) ||
      frame->abi != ALK_DS5_CORE_ABI_VERSION || frame->struct_size < sizeof(*frame) || frame->samples == NULL ||
      frame->channel_count != 4u || frame->sample_rate_hz != 48000u || frame->bits_per_sample != 16u) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  if (!core->session_active) {
    return ALK_DS5_ERROR_SESSION;
  }
  if (frame->device_index >= ALK_DS5_CORE_MAX_DEVICES ||
      frame->frame_count > DS5_AUDIO_QUEUE_FRAMES - core->audio_frames[frame->device_index]) {
    return ALK_DS5_ERROR_QUEUE_FULL;
  }
  for (uint32_t frame_index = 0u; frame_index < frame->frame_count; ++frame_index) {
    uint32_t slot = (core->audio_write[frame->device_index] + frame_index) % DS5_AUDIO_QUEUE_FRAMES;
    memcpy(&core->audio[frame->device_index][slot * 4u], &frame->samples[frame_index * 4u], sizeof(int16_t) * 4u);
  }
  core->audio_write[frame->device_index] =
      (core->audio_write[frame->device_index] + frame->frame_count) % DS5_AUDIO_QUEUE_FRAMES;
  core->audio_frames[frame->device_index] += frame->frame_count;
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_audio_read(AlkDs5Core *core, AlkDs5AudioFrame *frame) {
  if (core == NULL || frame == NULL || frame->struct_size < offsetof(AlkDs5AudioFrame, abi) + sizeof(frame->abi) ||
      frame->abi != ALK_DS5_CORE_ABI_VERSION || frame->struct_size < sizeof(*frame) || frame->output_samples == NULL ||
      frame->channel_count != 4u || frame->sample_rate_hz != 48000u || frame->bits_per_sample != 16u) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  if (!core->session_active) {
    return ALK_DS5_ERROR_SESSION;
  }
  if (frame->device_index >= ALK_DS5_CORE_MAX_DEVICES || frame->frame_count > core->audio_frames[frame->device_index]) {
    return ALK_DS5_ERROR_NOT_FOUND;
  }
  for (uint32_t frame_index = 0u; frame_index < frame->frame_count; ++frame_index) {
    uint32_t slot = (core->audio_read[frame->device_index] + frame_index) % DS5_AUDIO_QUEUE_FRAMES;
    memcpy(&frame->output_samples[frame_index * 4u], &core->audio[frame->device_index][slot * 4u],
           sizeof(int16_t) * 4u);
  }
  core->audio_read[frame->device_index] =
      (core->audio_read[frame->device_index] + frame->frame_count) % DS5_AUDIO_QUEUE_FRAMES;
  core->audio_frames[frame->device_index] -= frame->frame_count;
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_audio_flush(AlkDs5Core *core) {
  if (core == NULL) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  memcpy(core->audio_read, core->audio_write, sizeof(core->audio_read));
  memset(core->audio_frames, 0, sizeof(core->audio_frames));
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_submit_ir_haptics(AlkDs5Core *core, const AlkDs5IrHaptics *haptics) {
  if (core == NULL || !core->session_active || haptics == NULL ||
      haptics->struct_size < offsetof(AlkDs5IrHaptics, abi) + sizeof(haptics->abi) ||
      haptics->abi != ALK_DS5_CORE_ABI_VERSION || haptics->struct_size < sizeof(*haptics) ||
      haptics->device_index >= ALK_DS5_CORE_MAX_DEVICES) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  core->last_ir[haptics->device_index] = *haptics;
  core->ir_present[haptics->device_index] = true;
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_get_ir_haptics(const AlkDs5Core *core, uint32_t device_index, AlkDs5IrHaptics *haptics) {
  if (core == NULL || haptics == NULL || haptics->struct_size < offsetof(AlkDs5IrHaptics, abi) + sizeof(haptics->abi) ||
      haptics->abi != ALK_DS5_CORE_ABI_VERSION || haptics->struct_size < sizeof(*haptics) ||
      device_index >= ALK_DS5_CORE_MAX_DEVICES || !core->ir_present[device_index]) {
    return ALK_DS5_ERROR_NOT_FOUND;
  }
  *haptics = core->last_ir[device_index];
  return ALK_DS5_OK;
}

static AlkDs5Error validate_observation(const AlkDs5Core *core, uint32_t device_index, uint32_t struct_size,
                                        uint32_t abi, size_t required) {
  if (!valid_device(core, device_index) || abi != ALK_DS5_CORE_ABI_VERSION || struct_size < required) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  if (!core->present[device_index]) {
    return ALK_DS5_ERROR_STATE_ORDER;
  }
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_submit_touch(AlkDs5Core *core, const AlkDs5TouchObservation *observation) {
  if (observation == NULL || observation->contact_id > 1u || observation->active > 1u || observation->x > 1919u ||
      observation->y > 1079u || observation->pressure > 1023u) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  AlkDs5Error result = validate_observation(core, observation->device_index, observation->struct_size, observation->abi,
                                            sizeof(*observation));
  if (result != ALK_DS5_OK)
    return result;
  core->last_touch[observation->device_index][observation->contact_id] = *observation;
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_submit_motion(AlkDs5Core *core, const AlkDs5MotionObservation *observation) {
  if (observation == NULL)
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  AlkDs5Error result = validate_observation(core, observation->device_index, observation->struct_size, observation->abi,
                                            sizeof(*observation));
  if (result != ALK_DS5_OK)
    return result;
  core->last_motion[observation->device_index] = *observation;
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_submit_battery(AlkDs5Core *core, const AlkDs5BatteryObservation *observation) {
  if (observation == NULL || observation->level_steps > 10u || observation->charging_state > 3u ||
      observation->full > 1u) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  AlkDs5Error result = validate_observation(core, observation->device_index, observation->struct_size, observation->abi,
                                            sizeof(*observation));
  if (result != ALK_DS5_OK)
    return result;
  core->last_battery[observation->device_index] = *observation;
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_submit_jack(AlkDs5Core *core, const AlkDs5JackObservation *observation) {
  if (observation == NULL || observation->headset_present > 1u || observation->microphone_present > 1u ||
      observation->microphone_muted > 1u) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  AlkDs5Error result = validate_observation(core, observation->device_index, observation->struct_size, observation->abi,
                                            sizeof(*observation));
  if (result != ALK_DS5_OK)
    return result;
  core->last_jack[observation->device_index] = *observation;
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_submit_feedback(AlkDs5Core *core, const AlkDs5Feedback *feedback) {
  if (feedback == NULL)
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  AlkDs5Error result =
      validate_observation(core, feedback->device_index, feedback->struct_size, feedback->abi, sizeof(*feedback));
  if (result != ALK_DS5_OK)
    return result;
  uint8_t report[ALK_DS5_USB_OUTPUT_REPORT_BYTES] = {0};
  size_t report_length = 0;
  result = alk_ds5_build_output_report(feedback, report, sizeof(report), &report_length);
  if (result != ALK_DS5_OK || report_length != ALK_DS5_USB_OUTPUT_REPORT_BYTES) {
    return result == ALK_DS5_OK ? ALK_DS5_ERROR_MALFORMED_REPORT : result;
  }
  core->last_feedback[feedback->device_index] = *feedback;
  core->feedback_present[feedback->device_index] = true;
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_set_feature_report(AlkDs5Core *core, uint8_t report_id, const uint8_t *report, size_t length) {
  return alk_ds5_core_set_feature_report_for_device(core, 0u, report_id, report, length);
}

AlkDs5Error alk_ds5_core_set_feature_report_for_device(AlkDs5Core *core, uint32_t device_index, uint8_t report_id,
                                                       const uint8_t *report, size_t length) {
  size_t expected = 0u;
  if (core == NULL || device_index >= ALK_DS5_CORE_MAX_DEVICES || report == NULL ||
      length > sizeof(core->feature_data[0][0]) || alk_ds5_feature_report_length(report_id, &expected) != ALK_DS5_OK ||
      length != expected || report[0] != report_id) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  memcpy(core->feature_data[device_index][report_id], report, length);
  core->feature_lengths[device_index][report_id] = (uint8_t)length;
  core->feature_present[device_index][report_id] = true;
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_get_feature_report(const AlkDs5Core *core, uint8_t report_id, uint8_t *report, size_t capacity,
                                            size_t *length) {
  return alk_ds5_core_get_feature_report_for_device(core, 0u, report_id, report, capacity, length);
}

AlkDs5Error alk_ds5_core_get_feature_report_for_device(const AlkDs5Core *core, uint32_t device_index, uint8_t report_id,
                                                       uint8_t *report, size_t capacity, size_t *length) {
  if (core == NULL || device_index >= ALK_DS5_CORE_MAX_DEVICES || report == NULL || length == NULL ||
      !core->feature_present[device_index][report_id]) {
    return ALK_DS5_ERROR_NOT_FOUND;
  }
  if (capacity < core->feature_lengths[device_index][report_id]) {
    return ALK_DS5_ERROR_BUFFER_TOO_SMALL;
  }
  memcpy(report, core->feature_data[device_index][report_id], core->feature_lengths[device_index][report_id]);
  *length = core->feature_lengths[device_index][report_id];
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_set_stable_mac(AlkDs5Core *core, uint32_t device_index, const uint8_t mac[6]) {
  if (core == NULL || device_index >= ALK_DS5_CORE_MAX_DEVICES || mac == NULL) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  memcpy(core->stable_macs[device_index], mac, 6u);
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_core_get_stable_mac(const AlkDs5Core *core, uint32_t device_index, uint8_t mac[6]) {
  if (core == NULL || device_index >= ALK_DS5_CORE_MAX_DEVICES || mac == NULL) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  memcpy(mac, core->stable_macs[device_index], 6u);
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_parse_input_report(const uint8_t *report, size_t length, AlkDs5State *state) {
  if (report == NULL || state == NULL || state->struct_size < offsetof(AlkDs5State, abi) + sizeof(state->abi) ||
      state->abi != ALK_DS5_CORE_ABI_VERSION || state->struct_size < sizeof(*state)) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  if (length != ALK_DS5_USB_INPUT_REPORT_BYTES) {
    return ALK_DS5_ERROR_MALFORMED_REPORT;
  }
  if (report[0] != 0x01u) {
    return report[0] == 0x31u ? ALK_DS5_ERROR_UNSUPPORTED_REPORT : ALK_DS5_ERROR_MALFORMED_REPORT;
  }
  state->left_x = report[DS5_OFF_LEFT_X];
  state->left_y = report[DS5_OFF_LEFT_Y];
  state->right_x = report[DS5_OFF_RIGHT_X];
  state->right_y = report[DS5_OFF_RIGHT_Y];
  state->left_trigger = report[DS5_OFF_LEFT_TRIGGER];
  state->right_trigger = report[DS5_OFF_RIGHT_TRIGGER];
  state->hat = report[DS5_OFF_BUTTONS0] & 0x0fu;
  state->buttons = ((uint64_t)report[DS5_OFF_BUTTONS0] >> 4u) | ((uint64_t)report[DS5_OFF_BUTTONS1] << 4u) |
                   ((uint64_t)report[DS5_OFF_BUTTONS2] << 12u);
  state->buttons3 = report[DS5_OFF_BUTTONS3];
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_build_input_report(const AlkDs5State *state, uint8_t *report, size_t capacity, size_t *length) {
  if (state == NULL || report == NULL || length == NULL || state->abi != ALK_DS5_CORE_ABI_VERSION ||
      state->struct_size < sizeof(*state)) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  if (capacity < ALK_DS5_USB_INPUT_REPORT_BYTES) {
    return ALK_DS5_ERROR_BUFFER_TOO_SMALL;
  }
  memset(report, 0, ALK_DS5_USB_INPUT_REPORT_BYTES);
  report[0] = 0x01u;
  report[DS5_OFF_LEFT_X] = state->left_x;
  report[DS5_OFF_LEFT_Y] = state->left_y;
  report[DS5_OFF_RIGHT_X] = state->right_x;
  report[DS5_OFF_RIGHT_Y] = state->right_y;
  report[DS5_OFF_LEFT_TRIGGER] = state->left_trigger;
  report[DS5_OFF_RIGHT_TRIGGER] = state->right_trigger;
  report[DS5_OFF_BUTTONS0] = (uint8_t)((state->buttons << 4u) | (state->hat & 0x0fu));
  report[DS5_OFF_BUTTONS1] = (uint8_t)(state->buttons >> 4u);
  report[DS5_OFF_BUTTONS2] = (uint8_t)(state->buttons >> 12u);
  report[DS5_OFF_BUTTONS3] = state->buttons3;
  *length = ALK_DS5_USB_INPUT_REPORT_BYTES;
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_parse_output_report(const uint8_t *report, size_t length, uint32_t *valid_flags) {
  if (report == NULL || valid_flags == NULL) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  if ((length != ALK_DS5_USB_OUTPUT_REPORT_BYTES && length != 48u) || report[0] != 0x02u) {
    return ALK_DS5_ERROR_MALFORMED_REPORT;
  }
  *valid_flags = (uint32_t)report[ALK_DS5_OUTPUT_VALID_FLAG0_OFFSET] |
                 ((uint32_t)report[ALK_DS5_OUTPUT_VALID_FLAG1_OFFSET] << 8u) |
                 ((uint32_t)report[ALK_DS5_OUTPUT_VALID_FLAG2_OFFSET] << 16u);
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_parse_input_report_ex(const uint8_t *report, size_t length, AlkDs5ExtendedInput *input) {
  if (input == NULL || input->struct_size < offsetof(AlkDs5ExtendedInput, abi) + sizeof(input->abi) ||
      input->abi != ALK_DS5_CORE_ABI_VERSION || input->struct_size < sizeof(*input)) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  const uint32_t outer_size = input->struct_size;
  const uint32_t outer_abi = input->abi;
  memset(input, 0, sizeof(*input));
  input->struct_size = outer_size;
  input->abi = outer_abi;
  input->state.struct_size = sizeof(input->state);
  input->state.abi = ALK_DS5_CORE_ABI_VERSION;
  AlkDs5Error result = alk_ds5_parse_input_report(report, length, &input->state);
  if (result != ALK_DS5_OK) {
    return result;
  }
  memset(input->touch, 0, sizeof(input->touch));
  memset(&input->motion, 0, sizeof(input->motion));
  memset(&input->battery, 0, sizeof(input->battery));
  memset(&input->jack, 0, sizeof(input->jack));
  for (size_t index = 0u; index < 2u; ++index) {
    input->touch[index].struct_size = sizeof(input->touch[index]);
    input->touch[index].abi = ALK_DS5_CORE_ABI_VERSION;
  }
  input->motion.struct_size = sizeof(input->motion);
  input->motion.abi = ALK_DS5_CORE_ABI_VERSION;
  input->battery.struct_size = sizeof(input->battery);
  input->battery.abi = ALK_DS5_CORE_ABI_VERSION;
  input->jack.struct_size = sizeof(input->jack);
  input->jack.abi = ALK_DS5_CORE_ABI_VERSION;
  input->motion.gyro[0] = (int16_t)read_le16(report + 16u);
  input->motion.gyro[1] = (int16_t)read_le16(report + 18u);
  input->motion.gyro[2] = (int16_t)read_le16(report + 20u);
  input->motion.accelerometer[0] = (int16_t)read_le16(report + 22u);
  input->motion.accelerometer[1] = (int16_t)read_le16(report + 24u);
  input->motion.accelerometer[2] = (int16_t)read_le16(report + 26u);
  input->motion.sensor_timestamp = read_le32(report + 28u);
  const uint8_t *touch1 = report + 33u;
  const uint8_t *touch2 = report + 37u;
  const uint8_t *touch_data[2] = {touch1, touch2};
  for (size_t index = 0u; index < 2u; ++index) {
    const uint8_t *touch = touch_data[index];
    input->touch[index].contact_id = touch[0] & 0x7fu;
    input->touch[index].active = (touch[0] & 0x80u) == 0u;
    input->touch[index].x = (uint16_t)touch[1] | ((uint16_t)(touch[2] & 0x0fu) << 8u);
    input->touch[index].y = (uint16_t)(touch[2] >> 4u) | ((uint16_t)touch[3] << 4u);
  }
  input->battery.level_steps = report[53u] & 0x0fu;
  input->battery.charging_state = report[53u] >> 4u;
  input->battery.full = input->battery.charging_state == 2u;
  input->jack.headset_present = report[54u] & 0x01u;
  input->jack.microphone_present = (report[54u] >> 1u) & 0x01u;
  input->jack.microphone_muted = (report[54u] >> 2u) & 0x01u;
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_parse_output_report_ex(const uint8_t *report, size_t length, AlkDs5Feedback *feedback) {
  if (feedback == NULL || feedback->struct_size < offsetof(AlkDs5Feedback, abi) + sizeof(feedback->abi) ||
      feedback->abi != ALK_DS5_CORE_ABI_VERSION || feedback->struct_size < sizeof(*feedback)) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  uint32_t flags = 0u;
  AlkDs5Error result = alk_ds5_parse_output_report(report, length, &flags);
  if (result != ALK_DS5_OK) {
    return result;
  }
  feedback->valid_mask = flags;
  feedback->valid_flag0 = report[ALK_DS5_OUTPUT_VALID_FLAG0_OFFSET];
  feedback->valid_flag1 = report[ALK_DS5_OUTPUT_VALID_FLAG1_OFFSET];
  feedback->valid_flag2 = report[ALK_DS5_OUTPUT_VALID_FLAG2_OFFSET];
  feedback->low_frequency = report[4u];
  feedback->high_frequency = report[3u];
  /* Enable bits follow the Linux hid-playstation driver (DS_OUTPUT_VALID_FLAG0/1/2) and SDL's PS5 driver:
   * flag0 0x01 compatible vibration, 0x02 haptics select (no field here), 0x04 right trigger, 0x08 left
   * trigger, 0x10 headphone volume, 0x20 speaker volume, 0x40 microphone volume, 0x80 audio control;
   * flag1 0x01 mute LED, 0x02 microphone mute, 0x04 lightbar colour, 0x10 player LEDs, 0x80 audio control 2;
   * flag2 0x01 lightbar brightness, 0x04 improved rumble emulation (motor values on newer firmware). */
  if ((feedback->valid_flag0 & 0x01u) != 0u || (feedback->valid_flag2 & 0x04u) != 0u) {
    feedback->motor_right = report[3u];
    feedback->motor_left = report[4u];
  }
  if ((feedback->valid_flag0 & 0x10u) != 0u) {
    feedback->headphone_volume = report[5u];
  }
  if ((feedback->valid_flag0 & 0x20u) != 0u) {
    feedback->speaker_volume = report[6u];
  }
  if ((feedback->valid_flag0 & 0x40u) != 0u) {
    feedback->mic_volume = report[7u];
  }
  if ((feedback->valid_flag0 & 0x80u) != 0u) {
    feedback->audio_control = report[8u];
  }
  if ((feedback->valid_flag1 & 0x01u) != 0u) {
    feedback->mute_led = report[9u];
  }
  if ((feedback->valid_flag1 & 0x02u) != 0u) {
    feedback->mic_mute = (report[10u] & 0x10u) != 0u;
  }
  if ((feedback->valid_flag1 & 0x04u) != 0u) {
    feedback->lightbar_rgb[0] = report[45u];
    feedback->lightbar_rgb[1] = report[46u];
    feedback->lightbar_rgb[2] = report[47u];
  }
  if ((feedback->valid_flag2 & 0x01u) != 0u) {
    feedback->lightbar_brightness = report[43u];
  }
  if ((feedback->valid_flag1 & 0x10u) != 0u) {
    feedback->player_led_mask = report[44u];
  }
  if ((feedback->valid_flag0 & 0x04u) != 0u) {
    memcpy(feedback->right_trigger.raw, report + 11u, sizeof(feedback->right_trigger.raw));
    feedback->right_trigger.mode =
        feedback->right_trigger.raw[0] <= ALK_DS5_TRIGGER_RAW ? feedback->right_trigger.raw[0] : ALK_DS5_TRIGGER_RAW;
    feedback->right_trigger.start = feedback->right_trigger.raw[1];
    feedback->right_trigger.end = feedback->right_trigger.raw[2];
    feedback->right_trigger.strength = feedback->right_trigger.raw[3];
    feedback->right_trigger.frequency = feedback->right_trigger.raw[4];
  }
  if ((feedback->valid_flag0 & 0x08u) != 0u) {
    memcpy(feedback->left_trigger.raw, report + 22u, sizeof(feedback->left_trigger.raw));
    feedback->left_trigger.mode =
        feedback->left_trigger.raw[0] <= ALK_DS5_TRIGGER_RAW ? feedback->left_trigger.raw[0] : ALK_DS5_TRIGGER_RAW;
    feedback->left_trigger.start = feedback->left_trigger.raw[1];
    feedback->left_trigger.end = feedback->left_trigger.raw[2];
    feedback->left_trigger.strength = feedback->left_trigger.raw[3];
    feedback->left_trigger.frequency = feedback->left_trigger.raw[4];
  }
  if ((feedback->valid_flag1 & (1u << 7u)) != 0u) {
    feedback->audio_control2 = report[38u];
  }
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_build_output_report(const AlkDs5Feedback *feedback, uint8_t *report, size_t capacity,
                                        size_t *length) {
  if (feedback == NULL || report == NULL || length == NULL || feedback->abi != ALK_DS5_CORE_ABI_VERSION ||
      feedback->struct_size < sizeof(*feedback) || capacity < ALK_DS5_USB_OUTPUT_REPORT_BYTES) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  memset(report, 0, ALK_DS5_USB_OUTPUT_REPORT_BYTES);
  report[0] = 0x02u;
  report[1] = feedback->valid_flag0;
  report[2] = feedback->valid_flag1;
  report[3] = feedback->motor_right;
  report[4] = feedback->motor_left;
  report[5] = feedback->headphone_volume;
  report[6] = feedback->speaker_volume;
  report[7] = feedback->mic_volume;
  report[8] = feedback->audio_control;
  report[9] = feedback->mute_led;
  if (feedback->left_trigger.mode > ALK_DS5_TRIGGER_RAW || feedback->right_trigger.mode > ALK_DS5_TRIGGER_RAW ||
      feedback->left_trigger.start > feedback->left_trigger.end ||
      feedback->right_trigger.start > feedback->right_trigger.end) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  if (feedback->right_trigger.mode == ALK_DS5_TRIGGER_RAW) {
    memcpy(report + 11u, feedback->right_trigger.raw, sizeof(feedback->right_trigger.raw));
  } else {
    report[11u] = (uint8_t)feedback->right_trigger.mode;
    report[12u] = feedback->right_trigger.start;
    report[13u] = feedback->right_trigger.end;
    report[14u] = feedback->right_trigger.strength;
    report[15u] = feedback->right_trigger.frequency;
  }
  if (feedback->left_trigger.mode == ALK_DS5_TRIGGER_RAW) {
    memcpy(report + 22u, feedback->left_trigger.raw, sizeof(feedback->left_trigger.raw));
  } else {
    report[22u] = (uint8_t)feedback->left_trigger.mode;
    report[23u] = feedback->left_trigger.start;
    report[24u] = feedback->left_trigger.end;
    report[25u] = feedback->left_trigger.strength;
    report[26u] = feedback->left_trigger.frequency;
  }
  if ((feedback->valid_flag1 & (1u << 1u)) != 0u) {
    report[10u] = feedback->mic_mute ? 0x10u : 0u;
  }
  report[38u] = feedback->audio_control2;
  report[39u] = feedback->valid_flag2;
  report[43u] = feedback->lightbar_brightness;
  report[44u] = feedback->player_led_mask;
  report[45u] = feedback->lightbar_rgb[0];
  report[46u] = feedback->lightbar_rgb[1];
  report[47u] = feedback->lightbar_rgb[2];
  *length = ALK_DS5_USB_OUTPUT_REPORT_BYTES;
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_wrap_bluetooth_output_report(const uint8_t *usb_report, size_t usb_length, uint8_t *report,
                                                 size_t capacity, size_t *length) {
  return alk_ds5_wrap_bluetooth_output_report_seq(usb_report, usb_length, 0u, report, capacity, length);
}

AlkDs5Error alk_ds5_wrap_bluetooth_output_report_seq(const uint8_t *usb_report, size_t usb_length, uint8_t sequence,
                                                     uint8_t *report, size_t capacity, size_t *length) {
  if (usb_report == NULL || usb_length != ALK_DS5_USB_OUTPUT_REPORT_BYTES || report == NULL ||
      capacity < ALK_DS5_BT_OUTPUT_REPORT_BYTES || length == NULL || usb_report[0] != 0x02u) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  memset(report, 0, ALK_DS5_BT_OUTPUT_REPORT_BYTES);
  report[0] = 0x31u;
  /* High nibble is the sequence number, low nibble is the tag (zero). */
  report[1] = (uint8_t)((sequence & 0x0fu) << 4u);
  report[2] = 0x10u;
  memcpy(report + 3u, usb_report + 1u, ALK_DS5_USB_OUTPUT_REPORT_BYTES - 1u);
  const uint32_t crc = alk_ds5_crc32_seed(0xa2u, report, ALK_DS5_BT_OUTPUT_REPORT_BYTES - 4u);
  report[ALK_DS5_BT_OUTPUT_REPORT_BYTES - 4u] = (uint8_t)crc;
  report[ALK_DS5_BT_OUTPUT_REPORT_BYTES - 3u] = (uint8_t)(crc >> 8u);
  report[ALK_DS5_BT_OUTPUT_REPORT_BYTES - 2u] = (uint8_t)(crc >> 16u);
  report[ALK_DS5_BT_OUTPUT_REPORT_BYTES - 1u] = (uint8_t)(crc >> 24u);
  *length = ALK_DS5_BT_OUTPUT_REPORT_BYTES;
  return ALK_DS5_OK;
}

uint32_t alk_ds5_crc32(const uint8_t *data, size_t length) { return alk_ds5_crc32_seed(0u, data, length); }

uint32_t alk_ds5_crc32_seed(uint8_t seed, const uint8_t *data, size_t length) {
  uint32_t crc = UINT32_MAX;
  if (data == NULL && length != 0u) {
    return 0u;
  }
  crc ^= seed;
  for (uint32_t bit = 0u; bit < 8u; ++bit) {
    crc = (crc >> 1u) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
  }
  for (size_t index = 0u; index < length; ++index) {
    crc ^= data[index];
    for (uint32_t bit = 0u; bit < 8u; ++bit) {
      crc = (crc >> 1u) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
  }
  return ~crc;
}

AlkDs5Error alk_ds5_parse_feature_report(uint8_t report_id, const uint8_t *report, size_t length,
                                         AlkDs5FeatureFacts *facts) {
  size_t expected = 0u;
  if (facts == NULL || facts->struct_size < offsetof(AlkDs5FeatureFacts, abi) + sizeof(facts->abi) ||
      facts->abi != ALK_DS5_CORE_ABI_VERSION || facts->struct_size < sizeof(*facts) || report == NULL) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  if (alk_ds5_feature_report_length(report_id, &expected) != ALK_DS5_OK) {
    return ALK_DS5_ERROR_UNSUPPORTED_REPORT;
  }
  if (length != expected || report[0] != report_id) {
    return ALK_DS5_ERROR_MALFORMED_REPORT;
  }
  memset(facts, 0, sizeof(*facts));
  facts->struct_size = sizeof(*facts);
  facts->abi = ALK_DS5_CORE_ABI_VERSION;
  facts->report_id = report_id;
  if (report_id == ALK_DS5_FEATURE_PAIRING_ID) {
    memcpy(facts->stable_mac, report + 1u, sizeof(facts->stable_mac));
  } else if (report_id == ALK_DS5_FEATURE_FIRMWARE_ID) {
    if (length < 33u) {
      return ALK_DS5_ERROR_MALFORMED_REPORT;
    }
    memcpy(&facts->hardware_version, report + 24u, sizeof(uint32_t));
    memcpy(&facts->firmware_version, report + 28u, sizeof(uint32_t));
  } else if (report_id == ALK_DS5_FEATURE_CALIBRATION_ID) {
    if (length < 35u) {
      return ALK_DS5_ERROR_MALFORMED_REPORT;
    }
    memcpy(facts->calibration_values, report + 1u, sizeof(facts->calibration_values));
  }
  return ALK_DS5_OK;
}

AlkDs5Error alk_ds5_parse_bluetooth_input_report(const uint8_t *report, size_t length, AlkDs5ExtendedInput *input) {
  if (report == NULL || length != ALK_DS5_BT_INPUT_REPORT_BYTES || report[0] != 0x31u || input == NULL) {
    return ALK_DS5_ERROR_MALFORMED_REPORT;
  }
  if (length < 4u || alk_ds5_crc32_seed(0xa1u, report, length - 4u) !=
                         ((uint32_t)report[length - 4u] | ((uint32_t)report[length - 3u] << 8u) |
                          ((uint32_t)report[length - 2u] << 16u) | ((uint32_t)report[length - 1u] << 24u))) {
    return ALK_DS5_ERROR_BAD_CRC;
  }
  uint8_t normalized[ALK_DS5_USB_INPUT_REPORT_BYTES];
  normalized[0] = 0x01u;
  memcpy(normalized + 1u, report + 2u, sizeof(normalized) - 1u);
  return alk_ds5_parse_input_report_ex(normalized, sizeof(normalized), input);
}

AlkDs5Error alk_ds5_parse_bluetooth_output_report(const uint8_t *report, size_t length, AlkDs5Feedback *feedback) {
  if (report == NULL || length != ALK_DS5_BT_OUTPUT_REPORT_BYTES || report[0] != 0x31u || feedback == NULL) {
    return ALK_DS5_ERROR_MALFORMED_REPORT;
  }
  if (length < 4u || alk_ds5_crc32_seed(0xa2u, report, length - 4u) !=
                         ((uint32_t)report[length - 4u] | ((uint32_t)report[length - 3u] << 8u) |
                          ((uint32_t)report[length - 2u] << 16u) | ((uint32_t)report[length - 1u] << 24u))) {
    return ALK_DS5_ERROR_BAD_CRC;
  }
  uint8_t normalized[ALK_DS5_USB_OUTPUT_REPORT_BYTES];
  normalized[0] = 0x02u;
  memcpy(normalized + 1u, report + 3u, sizeof(normalized) - 1u);
  return alk_ds5_parse_output_report_ex(normalized, sizeof(normalized), feedback);
}

AlkDs5Error alk_ds5_feature_report_length(uint8_t report_id, size_t *length) {
  if (length == NULL) {
    return ALK_DS5_ERROR_INVALID_ARGUMENT;
  }
  switch (report_id) {
  case ALK_DS5_FEATURE_CALIBRATION_ID:
    *length = ALK_DS5_FEATURE_CALIBRATION_BYTES;
    return ALK_DS5_OK;
  case ALK_DS5_FEATURE_PAIRING_ID:
    *length = ALK_DS5_FEATURE_PAIRING_BYTES;
    return ALK_DS5_OK;
  case ALK_DS5_FEATURE_FIRMWARE_ID:
    *length = ALK_DS5_FEATURE_FIRMWARE_BYTES;
    return ALK_DS5_OK;
  default:
    return ALK_DS5_ERROR_UNSUPPORTED_REPORT;
  }
}
