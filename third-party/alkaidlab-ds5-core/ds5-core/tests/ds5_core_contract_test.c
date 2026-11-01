#include "alkaidlab/ds5_core/ds5_core.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int failures;

static const AlkDs5BackendEvidence g_evidence = {
    sizeof(g_evidence),
    ALK_DS5_CORE_ABI_VERSION,
    ALK_DS5_CAP_BUTTONS | ALK_DS5_CAP_STICKS | ALK_DS5_CAP_TOUCH | ALK_DS5_CAP_MOTION | ALK_DS5_CAP_BATTERY |
        ALK_DS5_CAP_RUMBLE | ALK_DS5_CAP_ADAPTIVE_TRIGGERS | ALK_DS5_CAP_LIGHTBAR | ALK_DS5_CAP_PLAYER_LIGHT |
        ALK_DS5_CAP_FEATURE_REPORTS | ALK_DS5_CAP_AUDIO_PCM | ALK_DS5_CAP_AUDIO_IR | ALK_DS5_CAP_SPEAKER |
        ALK_DS5_CAP_MICROPHONE | ALK_DS5_CAP_HEADSET_JACK | ALK_DS5_CAP_MIC_MUTE | ALK_DS5_CAP_USBIP_PROVIDER |
        ALK_DS5_CAP_USBIP_ISO | ALK_DS5_CAP_SESSION_LEASE,
    ALK_DS5_FEATURE_CALIBRATION | ALK_DS5_FEATURE_PAIRING | ALK_DS5_FEATURE_FIRMWARE,
    1u,
    0x01u,
    227u,
    273u,
    384u,
    192u,
    4u,
    {sizeof(AlkDs5AudioFacts), ALK_DS5_CORE_ABI_VERSION, 48u * 1000u, 16u, 4u, {1u, 2u, 3u, 4u}, 2400u, 1u, 1u, 1u},
    {0x02u, 0xdau, 0x55u, 0u, 0u, 1u},
    {0u, 0u}};

static void check(int condition, const char *name) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", name);
    ++failures;
  }
}

static AlkDs5Profile profile(void) {
  AlkDs5Profile value = {sizeof(value),
                         ALK_DS5_CORE_ABI_VERSION,
                         0x054c,
                         0x0ce6,
                         ALK_DS5_TRANSPORT_USB,
                         ALK_DS5_CORE_MAX_DEVICES,
                         NULL,
                         NULL,
                         "stage0",
                         &g_evidence};
  return value;
}

static AlkDs5Event event(void) {
  AlkDs5Event value;
  memset(&value, 0, sizeof(value));
  value.struct_size = sizeof(value);
  value.abi = ALK_DS5_CORE_ABI_VERSION;
  return value;
}

static void test_layout_and_facts(void) {
  check(offsetof(AlkDs5Profile, struct_size) == 0u, "profile starts with struct_size");
  check(offsetof(AlkDs5Profile, abi) == sizeof(uint32_t), "profile abi follows struct_size");
  check(offsetof(AlkDs5DeviceFacts, audio) > offsetof(AlkDs5DeviceFacts, failure_scope),
        "facts append audio at the end");

  AlkDs5Core *core = NULL;
  AlkDs5Profile p = profile();
  check(alk_ds5_core_create(&p, &core) == ALK_DS5_OK, "create USB profile");
  AlkDs5DeviceFacts facts;
  memset(&facts, 0, sizeof(facts));
  facts.struct_size = sizeof(facts);
  facts.abi = ALK_DS5_CORE_ABI_VERSION;
  check(alk_ds5_core_read_facts(core, &facts) == ALK_DS5_OK, "read facts");
  check((facts.capabilities & (ALK_DS5_CAP_TOUCH | ALK_DS5_CAP_MOTION | ALK_DS5_CAP_FEATURE_REPORTS)) != 0u,
        "semantic capabilities are advertised");
  check(facts.audio.sample_rate_hz == 48u * 1000u && facts.audio.channel_count == 4u &&
            facts.audio.supports_capture == 1u,
        "evidence-backed USB audio facts");
  alk_ds5_core_destroy(core);

  p.backend_evidence = NULL;
  core = NULL;
  check(alk_ds5_core_create(&p, &core) == ALK_DS5_OK, "create without backend evidence");
  memset(&facts, 0, sizeof(facts));
  facts.struct_size = sizeof(facts);
  facts.abi = ALK_DS5_CORE_ABI_VERSION;
  check(alk_ds5_core_read_facts(core, &facts) == ALK_DS5_OK && facts.capabilities == 0u &&
            facts.audio.channel_count == 0u,
        "unproven backend advertises no capabilities");
  alk_ds5_core_destroy(core);

  AlkDs5Profile short_profile = profile();
  short_profile.backend_evidence = &g_evidence;
  short_profile.struct_size = (uint32_t)(offsetof(AlkDs5Profile, serial) + sizeof(short_profile.serial));
  core = NULL;
  check(alk_ds5_core_create(&short_profile, &core) == ALK_DS5_OK, "accept older profile prefix");
  struct ShortFacts {
    uint32_t struct_size;
    uint32_t abi;
    uint32_t capabilities;
  } short_facts = {sizeof(short_facts), ALK_DS5_CORE_ABI_VERSION, 0u};
  check(alk_ds5_core_read_facts(core, (AlkDs5DeviceFacts *)&short_facts) == ALK_DS5_OK &&
            short_facts.capabilities == 0u,
        "write only requested facts prefix");
  alk_ds5_core_destroy(core);
}

static void test_two_controller_ordering(void) {
  AlkDs5Core *core = NULL;
  AlkDs5Profile p = profile();
  check(alk_ds5_core_create(&p, &core) == ALK_DS5_OK, "create ordering core");
  AlkDs5Event e = event();
  check(alk_ds5_core_arrive(core, 0u, &e) == ALK_DS5_OK && e.kind == ALK_DS5_EVENT_ARRIVAL, "controller zero arrives");
  check(alk_ds5_core_arrive(core, 1u, &e) == ALK_DS5_OK && e.kind == ALK_DS5_EVENT_ARRIVAL, "controller one arrives");

  AlkDs5State state = {sizeof(state), ALK_DS5_CORE_ABI_VERSION, 0u, 2u, 1u, 0u, 0u, 0u, 0u, 128u, 128u, 128u, 128u};
  check(alk_ds5_core_submit_state(core, &state, &e) == ALK_DS5_OK && e.kind == ALK_DS5_EVENT_STATE,
        "controller zero state follows arrival");
  state.device_index = 1u;
  state.sequence = 2u;
  check(alk_ds5_core_submit_state(core, &state, &e) == ALK_DS5_OK && e.device_index == 1u,
        "controller one state follows arrival");
  state.sequence = 2u;
  check(alk_ds5_core_submit_state(core, &state, &e) == ALK_DS5_ERROR_STATE_ORDER,
        "duplicate state sequence is rejected");
  check(alk_ds5_core_remove(core, 0u, &e) == ALK_DS5_OK && e.kind == ALK_DS5_EVENT_REMOVE, "controller zero removal");
  state.sequence = 3u;
  check(alk_ds5_core_submit_state(core, &state, &e) == ALK_DS5_OK, "controller one remains usable");

  size_t polled = 0u;
  while (alk_ds5_core_poll_event(core, &e) == ALK_DS5_OK) {
    ++polled;
  }
  check(polled == 6u, "arrival, state, and removal events are ordered");
  alk_ds5_core_destroy(core);
}

static void test_capability_loss(void) {
  AlkDs5Core *core = NULL;
  AlkDs5Profile p = profile();
  check(alk_ds5_core_create(&p, &core) == ALK_DS5_OK, "create loss core");
  AlkDs5CapabilityLoss loss = {sizeof(loss), ALK_DS5_CORE_ABI_VERSION};
  check(alk_ds5_core_describe_loss(core, ALK_DS5_CAP_TOUCH | ALK_DS5_CAP_AUDIO_RENDER, &loss) == ALK_DS5_OK,
        "describe requested capabilities");
  check(loss.lost == ALK_DS5_CAP_AUDIO_RENDER && loss.error_class == ALK_DS5_ERROR_CAPABILITY_UNAVAILABLE,
        "unproven capability is explicit loss");
  alk_ds5_core_destroy(core);
}

static void test_queue_failure_is_transactional(void) {
  AlkDs5Core *core = NULL;
  AlkDs5Profile p = profile();
  AlkDs5Event e = event();
  AlkDs5State state = {sizeof(state), ALK_DS5_CORE_ABI_VERSION, 0u, 2u, 0u, 8u, 0u, 0u, 0u, 128u, 128u, 128u, 128u, 0u};
  check(alk_ds5_core_create(&p, &core) == ALK_DS5_OK, "create queue core");
  check(alk_ds5_core_arrive(core, 0u, &e) == ALK_DS5_OK, "queue arrival zero");
  check(alk_ds5_core_arrive(core, 1u, &e) == ALK_DS5_OK, "queue arrival one");
  state.device_index = 0u;
  check(alk_ds5_core_submit_state(core, &state, &e) == ALK_DS5_OK, "queue state zero");
  state.device_index = 1u;
  check(alk_ds5_core_submit_state(core, &state, &e) == ALK_DS5_OK, "queue state one");
  state.sequence = 3u;
  check(alk_ds5_core_submit_state(core, &state, &e) == ALK_DS5_OK, "queue second state one");
  state.device_index = 0u;
  check(alk_ds5_core_submit_state(core, &state, &e) == ALK_DS5_OK, "queue second state zero");
  check(alk_ds5_core_remove(core, 0u, &e) == ALK_DS5_ERROR_QUEUE_FULL, "full queue rejects removal");
  check(alk_ds5_core_poll_event(core, &e) == ALK_DS5_OK, "drain one queued event");
  check(alk_ds5_core_remove(core, 0u, &e) == ALK_DS5_OK, "removal retries after drain");
  alk_ds5_core_destroy(core);
}

static void test_session_audio_ir_and_feature_scope(void) {
  AlkDs5Core *core = NULL;
  AlkDs5Profile p = profile();
  check(alk_ds5_core_create(&p, &core) == ALK_DS5_OK, "create scoped core");
  check(alk_ds5_core_begin_session(core, 42u) == ALK_DS5_OK, "begin session lease");
  AlkDs5IrHaptics ir = {sizeof(ir), ALK_DS5_CORE_ABI_VERSION, 1u, 100u, 200u, 300u, 400u, 99u};
  check(alk_ds5_core_submit_ir_haptics(core, &ir) == ALK_DS5_OK, "submit IR haptics payload");
  AlkDs5IrHaptics ir_read = {sizeof(ir_read), ALK_DS5_CORE_ABI_VERSION};
  check(alk_ds5_core_get_ir_haptics(core, 1u, &ir_read) == ALK_DS5_OK && ir_read.right_intensity == 200u,
        "get device-one IR haptics payload");
  int16_t in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  AlkDs5AudioFrame frame = {sizeof(frame), ALK_DS5_CORE_ABI_VERSION, 1u, 48u * 1000u, 16u, 4u, 0u, 2u, 0u, in, NULL};
  check(alk_ds5_core_audio_write(core, &frame) == ALK_DS5_OK, "write device-one audio");
  int16_t out[8] = {0};
  frame.output_samples = out;
  check(alk_ds5_core_audio_read(core, &frame) == ALK_DS5_OK && memcmp(in, out, sizeof(in)) == 0,
        "read device-one audio independently");
  uint8_t feature[64] = {0x20u};
  uint8_t feature_out[64] = {0};
  size_t feature_length = 0u;
  check(alk_ds5_core_set_feature_report(core, 0x20u, feature, sizeof(feature)) == ALK_DS5_OK &&
            alk_ds5_core_get_feature_report(core, 0x20u, feature_out, sizeof(feature_out), &feature_length) ==
                ALK_DS5_OK &&
            feature_length == sizeof(feature) && memcmp(feature, feature_out, sizeof(feature)) == 0,
        "generic feature report set/get");
  feature[1] = 0x7fu;
  uint8_t feature_one[64] = {0};
  check(alk_ds5_core_set_feature_report_for_device(core, 1u, 0x20u, feature, sizeof(feature)) == ALK_DS5_OK &&
            alk_ds5_core_get_feature_report_for_device(core, 1u, 0x20u, feature_one, sizeof(feature_one),
                                                       &feature_length) == ALK_DS5_OK &&
            feature_one[1] == 0x7fu && feature_out[1] == 0u,
        "feature reports remain per-device");
  const uint8_t mac_one[6] = {1u, 2u, 3u, 4u, 5u, 6u};
  uint8_t mac_read[6] = {0};
  check(alk_ds5_core_set_stable_mac(core, 1u, mac_one) == ALK_DS5_OK &&
            alk_ds5_core_get_stable_mac(core, 1u, mac_read) == ALK_DS5_OK && memcmp(mac_one, mac_read, 6u) == 0,
        "stable MAC remains per-device");
  check(alk_ds5_core_end_session(core, 42u) == ALK_DS5_OK &&
            alk_ds5_core_audio_read(core, &frame) == ALK_DS5_ERROR_SESSION,
        "session end clears scoped audio");
  alk_ds5_core_destroy(core);
}

static void test_high_level_observations_and_feedback(void) {
  AlkDs5Core *core = NULL;
  AlkDs5Profile p = profile();
  AlkDs5Event e = event();
  check(alk_ds5_core_create(&p, &core) == ALK_DS5_OK, "create observation core");
  check(alk_ds5_core_arrive(core, 0u, &e) == ALK_DS5_OK, "observation device arrives");
  AlkDs5TouchObservation touch = {sizeof(touch), ALK_DS5_CORE_ABI_VERSION, 0u, 1u, 0u, 1u, 960u, 540u, 512u};
  AlkDs5MotionObservation motion = {sizeof(motion), ALK_DS5_CORE_ABI_VERSION, 0u, 1u, {1, 2, 3}, {4, 5, 6}, 7u};
  AlkDs5BatteryObservation battery = {sizeof(battery), ALK_DS5_CORE_ABI_VERSION, 0u, 1u, 8u, 1u, 0u, 0u};
  AlkDs5JackObservation jack = {sizeof(jack), ALK_DS5_CORE_ABI_VERSION, 0u, 1u, 1u, 1u, 1u, 0u};
  check(alk_ds5_core_submit_touch(core, &touch) == ALK_DS5_OK, "submit touch observation");
  check(alk_ds5_core_submit_motion(core, &motion) == ALK_DS5_OK, "submit motion observation");
  check(alk_ds5_core_submit_battery(core, &battery) == ALK_DS5_OK, "submit battery observation");
  check(alk_ds5_core_submit_jack(core, &jack) == ALK_DS5_OK, "submit jack observation");
  AlkDs5Feedback feedback = {sizeof(feedback), ALK_DS5_CORE_ABI_VERSION};
  feedback.device_index = 0u;
  feedback.valid_flag0 = 1u;
  feedback.motor_left = 0x40u;
  feedback.motor_right = 0x20u;
  check(alk_ds5_core_submit_feedback(core, &feedback) == ALK_DS5_OK, "submit validated feedback");
  touch.x = 1920u;
  check(alk_ds5_core_submit_touch(core, &touch) == ALK_DS5_ERROR_INVALID_ARGUMENT, "reject invalid touch bounds");
  alk_ds5_core_destroy(core);
}

int main(void) {
  test_layout_and_facts();
  test_two_controller_ordering();
  test_capability_loss();
  test_queue_failure_is_transactional();
  test_session_audio_ir_and_feature_scope();
  test_high_level_observations_and_feedback();
  if (failures != 0) {
    fprintf(stderr, "ds5_core_contract_test: %d failure(s)\n", failures);
    return 1;
  }
  puts("ds5_core_contract_test: all checks passed");
  return 0;
}
