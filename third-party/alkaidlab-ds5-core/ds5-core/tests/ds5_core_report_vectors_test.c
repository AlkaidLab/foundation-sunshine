#include "alkaidlab/ds5_core/ds5_core.h"

#include <stdio.h>
#include <string.h>

static int failures;

static size_t load_hex(const char *name, uint8_t *out, size_t capacity) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", DS5_FIXTURE_DIR, name);
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return 0u;
  }
  size_t count = 0u;
  unsigned value = 0u;
  while (fscanf(file, "%2x", &value) == 1 && count < capacity) {
    out[count++] = (uint8_t)value;
  }
  fclose(file);
  return count;
}

static void check(int condition, const char *name) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", name);
    ++failures;
  }
}

static AlkDs5State state(void) {
  AlkDs5State value = {sizeof(value), ALK_DS5_CORE_ABI_VERSION, 0u, 1u, 0x31u, 2u, 3u, 4u, 0u, 11u, 22u, 33u, 44u};
  return value;
}

static void test_usb_round_trip(void) {
  uint8_t report[ALK_DS5_USB_INPUT_REPORT_BYTES];
  size_t length = 0u;
  AlkDs5State input = state();
  check(load_hex("spikeA-input-usb.hex", report, sizeof(report)) == sizeof(report), "load captured USB input");
  AlkDs5State captured = {sizeof(captured), ALK_DS5_CORE_ABI_VERSION};
  check(alk_ds5_parse_input_report(report, sizeof(report), &captured) == ALK_DS5_OK, "parse captured USB input");
  check(captured.hat == 8u && captured.buttons == 0u && report[7] == 0x70u && report[8] == 0x08u,
        "captured sequence and button offsets");
  check(alk_ds5_build_input_report(&input, report, sizeof(report), &length) == ALK_DS5_OK, "build USB report vector");
  check(length == ALK_DS5_USB_INPUT_REPORT_BYTES && report[0] == 0x01u, "USB report length and ID");
  AlkDs5State output = {sizeof(output), ALK_DS5_CORE_ABI_VERSION};
  check(alk_ds5_parse_input_report(report, length, &output) == ALK_DS5_OK, "parse USB report vector");
  check(output.buttons == input.buttons && output.hat == input.hat && output.left_x == input.left_x &&
            output.right_trigger == input.right_trigger,
        "USB report state round trip");

  uint8_t malformed[ALK_DS5_USB_INPUT_REPORT_BYTES];
  memcpy(malformed, report, sizeof(malformed));
  malformed[0] = 0x31u;
  output = (AlkDs5State){sizeof(output), ALK_DS5_CORE_ABI_VERSION};
  check(alk_ds5_parse_input_report(malformed, sizeof(malformed), &output) == ALK_DS5_ERROR_UNSUPPORTED_REPORT,
        "Bluetooth report is not accepted by USB parser");
  check(alk_ds5_parse_input_report(report, 0u, &output) == ALK_DS5_ERROR_MALFORMED_REPORT,
        "zero-length input is rejected safely");
}

/* Enable-bit vectors from the Linux hid-playstation driver (DS_OUTPUT_VALID_FLAG0/1/2) and SDL's PS5
 * driver (ucEnableBits1/2/3). Each case sets one enable bit over a report whose value bytes are all
 * distinct and non-zero, and checks which fields the parser fills: exactly the ones that bit enables. */
typedef struct {
  const char *name;
  uint8_t flag0, flag1, flag2;
  uint8_t motors, headphone, speaker, mic, audio_control, mute_led, mic_mute, rgb, brightness, player, audio_control2;
  uint8_t right_trigger, left_trigger;
} OutputFlagCase;

static void test_output_enable_bits(void) {
  static const OutputFlagCase cases[] = {
      {"flag0 0x01 compatible vibration", 0x01, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
      {"flag0 0x02 haptics select enables no field", 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
      {"flag0 0x04 right trigger", 0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0},
      {"flag0 0x08 left trigger", 0x08, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
      {"flag0 0x10 headphone volume", 0x10, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
      {"flag0 0x20 speaker volume", 0x20, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
      {"flag0 0x40 microphone volume", 0x40, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
      {"flag0 0x80 audio control", 0x80, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
      {"flag1 0x01 mute LED", 0, 0x01, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
      {"flag1 0x02 microphone mute", 0, 0x02, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
      {"flag1 0x04 lightbar colour only", 0, 0x04, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0},
      {"flag1 0x08 release LEDs enables no field", 0, 0x08, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
      {"flag1 0x10 player LEDs", 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0},
      {"flag1 0x80 audio control 2", 0, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0},
      {"flag2 0x01 lightbar brightness", 0, 0, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0},
      {"flag2 0x02 lightbar setup enables no field", 0, 0, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
      {"flag2 0x04 improved rumble emulation", 0, 0, 0x04, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
  };
  for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    const OutputFlagCase *c = &cases[index];
    uint8_t report[ALK_DS5_USB_OUTPUT_REPORT_BYTES] = {0};
    report[0] = 0x02u;
    report[1] = c->flag0;
    report[2] = c->flag1;
    report[3] = 0x11u;  /* motor right */
    report[4] = 0x22u;  /* motor left */
    report[5] = 0x33u;  /* headphone volume */
    report[6] = 0x44u;  /* speaker volume */
    report[7] = 0x55u;  /* microphone volume */
    report[8] = 0x66u;  /* audio control */
    report[9] = 0x01u;  /* mute LED */
    report[10] = 0x10u; /* microphone mute */
    report[11] = ALK_DS5_TRIGGER_FEEDBACK;
    report[12] = 0x21u;
    report[22] = ALK_DS5_TRIGGER_WEAPON;
    report[23] = 0x31u;
    report[38] = 0x1au; /* audio control 2 */
    report[39] = c->flag2;
    report[43] = 0x02u; /* lightbar brightness */
    report[44] = 0x15u; /* player LEDs */
    report[45] = 0x71u;
    report[46] = 0x72u;
    report[47] = 0x73u;
    AlkDs5Feedback f = {sizeof(f), ALK_DS5_CORE_ABI_VERSION};
    check(alk_ds5_parse_output_report_ex(report, sizeof(report), &f) == ALK_DS5_OK, c->name);
    check((f.motor_right == 0x11u && f.motor_left == 0x22u) == (c->motors != 0u) &&
              (c->motors != 0u || (f.motor_right == 0u && f.motor_left == 0u)),
          c->name);
    check((f.headphone_volume == 0x33u) == (c->headphone != 0u) && (c->headphone != 0u || f.headphone_volume == 0u),
          c->name);
    check((f.speaker_volume == 0x44u) == (c->speaker != 0u) && (c->speaker != 0u || f.speaker_volume == 0u), c->name);
    check((f.mic_volume == 0x55u) == (c->mic != 0u) && (c->mic != 0u || f.mic_volume == 0u), c->name);
    check((f.audio_control == 0x66u) == (c->audio_control != 0u) && (c->audio_control != 0u || f.audio_control == 0u),
          c->name);
    check((f.mute_led == 1u) == (c->mute_led != 0u), c->name);
    check((f.mic_mute != 0u) == (c->mic_mute != 0u), c->name);
    check((f.lightbar_rgb[0] == 0x71u && f.lightbar_rgb[2] == 0x73u) == (c->rgb != 0u) &&
              (c->rgb != 0u || (f.lightbar_rgb[0] == 0u && f.lightbar_rgb[1] == 0u && f.lightbar_rgb[2] == 0u)),
          c->name);
    check((f.lightbar_brightness == 0x02u) == (c->brightness != 0u), c->name);
    check((f.player_led_mask == 0x15u) == (c->player != 0u), c->name);
    check((f.audio_control2 == 0x1au) == (c->audio_control2 != 0u), c->name);
    check((f.right_trigger.mode == ALK_DS5_TRIGGER_FEEDBACK && f.right_trigger.start == 0x21u) ==
              (c->right_trigger != 0u),
          c->name);
    check((f.left_trigger.mode == ALK_DS5_TRIGGER_WEAPON && f.left_trigger.start == 0x31u) == (c->left_trigger != 0u),
          c->name);
  }
}

static void test_output_and_feature_vectors(void) {
  uint8_t descriptor[300];
  check(load_hex("cfg_desc.hex", descriptor, sizeof(descriptor)) == 227u, "captured configuration descriptor");
  check(load_hex("hid_report_desc.hex", descriptor, sizeof(descriptor)) == 273u, "captured HID report descriptor");
  uint8_t captured_output[ALK_DS5_USB_OUTPUT_REPORT_BYTES];
  uint8_t output[ALK_DS5_USB_OUTPUT_REPORT_BYTES];
  check(load_hex("spikeA-output-usb-63.hex", captured_output, sizeof(captured_output)) == sizeof(captured_output),
        "load synthetic USB output shape");
  memcpy(output, captured_output, sizeof(output));
  output[1] = 0x01u;
  output[2] = 0x02u;
  output[39] = 0x04u;
  uint32_t flags = 0u;
  check(alk_ds5_parse_output_report(output, sizeof(output), &flags) == ALK_DS5_OK && flags == 0x040201u,
        "output valid flags vector");
  uint8_t short_output[48];
  memcpy(short_output, captured_output, sizeof(short_output));
  check(alk_ds5_parse_output_report(short_output, sizeof(short_output), &flags) == ALK_DS5_OK,
        "captured Windows report length remains accepted");
  short_output[2] |= 0x80u; /* valid_flag1 bit 7 enables audio_control2 */
  short_output[38] = 0x1au;
  short_output[39] = 0x04u;
  AlkDs5Feedback short_feedback = {sizeof(short_feedback), ALK_DS5_CORE_ABI_VERSION};
  check(alk_ds5_parse_output_report_ex(short_output, sizeof(short_output), &short_feedback) == ALK_DS5_OK &&
            short_feedback.valid_flag2 == 4u && short_feedback.audio_control2 == 0x1au,
        "48-byte output keeps valid_flag2 and audio_control2");
  AlkDs5Feedback feedback = {sizeof(feedback), ALK_DS5_CORE_ABI_VERSION};
  feedback.valid_flag0 = 1u;
  feedback.valid_flag1 = 0x14u;
  feedback.valid_flag2 = 4u;
  feedback.motor_right = 0x55u;
  feedback.motor_left = 0xaau;
  feedback.lightbar_rgb[0] = 0x12u;
  feedback.lightbar_rgb[1] = 0x34u;
  feedback.lightbar_rgb[2] = 0x56u;
  size_t output_length = 0u;
  check(alk_ds5_build_output_report(&feedback, output, sizeof(output), &output_length) == ALK_DS5_OK &&
            output_length == 63u && output[39] == 4u,
        "output builder pins 63-byte valid flags");
  AlkDs5Feedback parsed_feedback = {sizeof(parsed_feedback), ALK_DS5_CORE_ABI_VERSION};
  check(alk_ds5_parse_output_report_ex(output, output_length, &parsed_feedback) == ALK_DS5_OK &&
            parsed_feedback.motor_right == 0x55u && parsed_feedback.motor_left == 0xaau &&
            parsed_feedback.lightbar_rgb[2] == 0x56u,
        "output parser decodes independent feedback values");
  size_t length = 0u;
  check(alk_ds5_feature_report_length(0x05u, &length) == ALK_DS5_OK && length == 41u, "calibration feature vector");
  check(alk_ds5_feature_report_length(0x09u, &length) == ALK_DS5_OK && length == 20u, "pairing feature vector");
  check(alk_ds5_feature_report_length(0x20u, &length) == ALK_DS5_OK && length == 64u, "firmware feature vector");
  check(alk_ds5_feature_report_length(0x99u, &length) == ALK_DS5_ERROR_UNSUPPORTED_REPORT,
        "unknown feature report rejected");
  uint8_t feature[64];
  AlkDs5FeatureFacts facts = {sizeof(facts), ALK_DS5_CORE_ABI_VERSION};
  size_t feature_length = load_hex("spikeA-feature-09.hex", feature, sizeof(feature));
  check(feature_length == 20u && alk_ds5_parse_feature_report(0x09u, feature, feature_length, &facts) == ALK_DS5_OK &&
            facts.stable_mac[0] == 0x02u && facts.stable_mac[1] == 0xdau,
        "captured pairing payload");
  feature_length = load_hex("spikeA-feature-20.hex", feature, sizeof(feature));
  facts = (AlkDs5FeatureFacts){sizeof(facts), ALK_DS5_CORE_ABI_VERSION};
  check(feature_length == 64u && alk_ds5_parse_feature_report(0x20u, feature, feature_length, &facts) == ALK_DS5_OK &&
            facts.hardware_version == 0x00010000u && facts.firmware_version == 0x00010000u,
        "captured firmware payload");

  uint8_t bluetooth[78];
  AlkDs5ExtendedInput extended = {sizeof(extended), ALK_DS5_CORE_ABI_VERSION};
  size_t bluetooth_length = load_hex("spikeA-bt-input-synthetic.hex", bluetooth, sizeof(bluetooth));
  check(bluetooth_length == sizeof(bluetooth) &&
            alk_ds5_parse_bluetooth_input_report(bluetooth, bluetooth_length, &extended) == ALK_DS5_OK &&
            extended.state.left_x == 128u && extended.state.hat == 8u,
        "synthetic Bluetooth input CRC and offsets");
  AlkDs5Feedback bluetooth_feedback = {sizeof(bluetooth_feedback), ALK_DS5_CORE_ABI_VERSION};
  bluetooth_length = load_hex("spikeA-bt-output-synthetic.hex", bluetooth, sizeof(bluetooth));
  check(bluetooth_length == sizeof(bluetooth) &&
            alk_ds5_parse_bluetooth_output_report(bluetooth, bluetooth_length, &bluetooth_feedback) == ALK_DS5_OK &&
            bluetooth_feedback.valid_flag0 == 1u && bluetooth_feedback.motor_right == 0x55u &&
            bluetooth_feedback.motor_left == 0xaau,
        "synthetic Bluetooth output CRC and common offsets");
  uint8_t wrapped_bluetooth[78] = {0};
  size_t wrapped_length = 0u;
  check(alk_ds5_wrap_bluetooth_output_report(output, output_length, wrapped_bluetooth, sizeof(wrapped_bluetooth),
                                             &wrapped_length) == ALK_DS5_OK &&
            wrapped_length == sizeof(wrapped_bluetooth) &&
            alk_ds5_parse_bluetooth_output_report(wrapped_bluetooth, wrapped_length, &bluetooth_feedback) == ALK_DS5_OK,
        "Bluetooth output wrapper preserves the reviewed CRC and layout");
  check(wrapped_bluetooth[1] == 0x00u, "Bluetooth output wrapper starts at sequence zero");

  for (uint8_t sequence = 0u; sequence < 16u; ++sequence) {
    uint8_t sequenced[ALK_DS5_BT_OUTPUT_REPORT_BYTES] = {0};
    size_t sequenced_length = 0u;
    check(alk_ds5_wrap_bluetooth_output_report_seq(output, output_length, sequence, sequenced, sizeof(sequenced),
                                                   &sequenced_length) == ALK_DS5_OK &&
              sequenced_length == sizeof(sequenced) && sequenced[1] == (uint8_t)(sequence << 4u) &&
              memcmp(sequenced + 3u, wrapped_bluetooth + 3u, ALK_DS5_BT_OUTPUT_REPORT_BYTES - 3u - 4u) == 0u &&
              alk_ds5_parse_bluetooth_output_report(sequenced, sequenced_length, &bluetooth_feedback) == ALK_DS5_OK,
          "Bluetooth output sequence advances in the high nibble");
  }
  {
    uint8_t wrapped[ALK_DS5_BT_OUTPUT_REPORT_BYTES] = {0};
    size_t wrapped_size = 0u;
    check(alk_ds5_wrap_bluetooth_output_report_seq(output, output_length, 0xffu, wrapped, sizeof(wrapped),
                                                   &wrapped_size) == ALK_DS5_OK &&
              wrapped_size == sizeof(wrapped) && wrapped[1] == 0xf0u,
          "Bluetooth output sequence keeps only the low four bits");
    check(alk_ds5_wrap_bluetooth_output_report_seq(output, output_length, 1u, wrapped, sizeof(wrapped) - 1u,
                                                   &wrapped_size) == ALK_DS5_ERROR_INVALID_ARGUMENT,
          "Bluetooth output sequence rejects a short buffer");
  }

  uint8_t extended_report[64];
  extended = (AlkDs5ExtendedInput){sizeof(extended), ALK_DS5_CORE_ABI_VERSION};
  check(load_hex("spikeA-input-extended-synthetic.hex", extended_report, sizeof(extended_report)) == 64u &&
            alk_ds5_parse_input_report_ex(extended_report, sizeof(extended_report), &extended) == ALK_DS5_OK &&
            extended.motion.gyro[0] == 0x1234 && extended.touch[0].active && extended.touch[0].x == 0x234u &&
            extended.touch[0].y == 0x456u && extended.battery.level_steps == 1u && extended.jack.headset_present &&
            extended.jack.microphone_muted,
        "synthetic extended input values");
}

int main(void) {
  test_usb_round_trip();
  test_output_and_feature_vectors();
  test_output_enable_bits();
  if (failures != 0) {
    fprintf(stderr, "ds5_core_report_vectors_test: %d failure(s)\n", failures);
    return 1;
  }
  puts("ds5_core_report_vectors_test: all checks passed");
  return 0;
}
