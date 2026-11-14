#include "alkaidlab/usbip_core/usbip_core.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int failures;
static void check(int condition, const char *name) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", name);
    ++failures;
  }
}

static void put_u32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24u);
  p[1] = (uint8_t)(value >> 16u);
  p[2] = (uint8_t)(value >> 8u);
  p[3] = (uint8_t)value;
}

static void test_submit_and_unlink(void) {
  uint8_t submit[48] = {0};
  put_u32(submit, ALK_USBIP_CMD_SUBMIT);
  put_u32(submit + 4u, 17u);
  put_u32(submit + 8u, 0x1000u);
  put_u32(submit + 12u, 1u);
  put_u32(submit + 16u, 0x84u);
  put_u32(submit + 24u, 64u);
  put_u32(submit + 28u, 0u);
  put_u32(submit + 32u, 0xffffffffu);
  put_u32(submit + 36u, 4u);
  AlkUsbIpUrb urb = {sizeof(urb), ALK_USBIP_CORE_ABI_VERSION};
  check(alk_usbip_parse_submit(submit, sizeof(submit), &urb) == ALK_USBIP_OK, "parse submit vector");
  check(urb.sequence == 17u && urb.endpoint == 0x84u && urb.transfer_buffer_length == 64u,
        "submit fields are decoded in network order");

  AlkUsbIpRequestTracker *tracker = NULL;
  check(alk_usbip_request_tracker_create(&tracker) == ALK_USBIP_OK, "create tracker");
  check(alk_usbip_request_tracker_submit(tracker, &urb) == ALK_USBIP_OK, "track submit");
  check(alk_usbip_request_tracker_is_active(tracker, 17u), "submit is active");
  AlkUsbIpCompletion completion = {sizeof(completion), ALK_USBIP_CORE_ABI_VERSION};
  check(alk_usbip_request_tracker_unlink(tracker, 17u, &completion) == ALK_USBIP_OK, "unlink active submit");
  check(completion.status == -104 && completion.submit_suppressed && !alk_usbip_request_tracker_is_active(tracker, 17u),
        "unlink returns ECONNRESET and suppresses RET_SUBMIT");
  check(alk_usbip_request_tracker_unlink(tracker, 17u, &completion) == ALK_USBIP_ERROR_NOT_FOUND,
        "second unlink is rejected");
  completion = (AlkUsbIpCompletion){sizeof(completion), ALK_USBIP_CORE_ABI_VERSION};
  check(alk_usbip_request_tracker_submit(tracker, &urb) == ALK_USBIP_OK, "track normal completion");
  check(alk_usbip_request_tracker_complete(tracker, 17u, &completion) == ALK_USBIP_OK &&
            !completion.submit_suppressed && !alk_usbip_request_tracker_is_active(tracker, 17u),
        "normal completion retires submit");
  uint8_t ret[64];
  size_t ret_length = 0u;
  check(alk_usbip_build_ret_submit(&completion, NULL, 0u, ret, sizeof(ret), &ret_length) == ALK_USBIP_OK &&
            ret_length == 48u,
        "RET_SUBMIT vector builds");
  check(alk_usbip_build_ret_unlink(19u, -104, ret, sizeof(ret), &ret_length) == ALK_USBIP_OK && ret_length == 48u,
        "RET_UNLINK vector builds");
  completion.sequence = 19u;
  completion.device_id = 0x1000u;
  completion.endpoint = 0x84u;
  completion.status = -104;
  check(alk_usbip_build_ret_unlink_ex(&completion, ret, sizeof(ret), &ret_length) == ALK_USBIP_OK &&
            ret_length == 48u && ret[0] == 0u && ret[3] == 4u && ret[8] == 0u && ret[12] == 0u && ret[16] == 0u,
        "RET_UNLINK zeros server basic header fields");
  alk_usbip_request_tracker_destroy(tracker);
}

static void test_iso_and_audio_controls(void) {
  uint8_t submit[48u + 384u + 16u] = {0};
  put_u32(submit, ALK_USBIP_CMD_SUBMIT);
  put_u32(submit + 4u, 21u);
  put_u32(submit + 12u, 0u);
  put_u32(submit + 16u, 1u);
  put_u32(submit + 24u, 384u);
  put_u32(submit + 32u, 1u);
  put_u32(submit + 36u, 4u);
  put_u32(submit + 48u + 384u + 4u, 384u);
  AlkUsbIpIsoPacket packet;
  AlkUsbIpUrb urb = {sizeof(urb), ALK_USBIP_CORE_ABI_VERSION, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, {0}, &packet, 1u};
  check(alk_usbip_parse_submit(submit, sizeof(submit), &urb) == ALK_USBIP_OK && packet.length == 384u,
        "compact ISO descriptor vector");

  AlkUsbIpIsoPacer pacer = {sizeof(pacer), ALK_USBIP_CORE_ABI_VERSION};
  uint64_t due = 0u;
  check(alk_usbip_iso_pacer_init(&pacer, 4u) == ALK_USBIP_OK &&
            alk_usbip_iso_pacer_next(&pacer, 100u, 10u, &due) == ALK_USBIP_OK && due == 100u &&
            pacer.next_due_ns == 100u + 10u * ALK_USBIP_DS5_ISO_PACKET_NS,
        "one millisecond ISO pacing vector");

  AlkUsbIpUacFeatureUnit unit = {sizeof(unit), ALK_USBIP_CORE_ABI_VERSION, 2u, 0u, 0u, -25600, -25600, 0, 256};
  int32_t value = 0;
  check(alk_usbip_uac_feature_get(&unit, 2u, 0x82u, &value) == ALK_USBIP_OK && value == -25600,
        "speaker GET_MIN vector");
  check(alk_usbip_uac_feature_set(&unit, 1u, 1) == ALK_USBIP_OK && unit.mute == 1u, "speaker mute SET_CUR vector");
  AlkUsbIpUacFeatureUnit microphone = {sizeof(microphone), ALK_USBIP_CORE_ABI_VERSION, 5u, 0u, 0u, 3809, 0, 12288, 122};
  check(alk_usbip_uac_feature_get(&microphone, 2u, 0x81u, &value) == ALK_USBIP_OK && value == 3809,
        "microphone GET_CUR vector");
  check(alk_usbip_uac_feature_set(&microphone, 2u, 1220) == ALK_USBIP_OK && microphone.volume_current == 1220,
        "microphone volume SET_CUR vector");
}

static unsigned listener_events;
static void on_listener_event(void *context, AlkUsbIpListenerEvent event, const AlkUsbIpUrb *urb,
                              const AlkUsbIpCompletion *completion) {
  (void)context;
  (void)urb;
  (void)completion;
  listener_events |= 1u << event;
}

static AlkUsbIpError provider_submit(void *context, const AlkUsbIpUrb *urb, AlkUsbIpCompletion *completion) {
  (void)context;
  completion->sequence = urb->sequence;
  completion->status = 0;
  return ALK_USBIP_OK;
}

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24u) | ((uint32_t)p[1] << 16u) | ((uint32_t)p[2] << 8u) | p[3];
}

/* Wire vectors laid out like the ones usbip-win2 (vadimgrn, drivers/ude/proto.cpp, include/usbip/proto.h)
 * puts on the wire. They are reconstructed from that sender's header layout, not captured packets. */
static void test_real_sender_vectors(void) {
  /* CMD_UNLINK: base header, then unlink_seqnum, then the unused tail of the header union. The tail
   * holds what the previous CMD_SUBMIT left in the same buffer (flags, length, start_frame, packets,
   * interval, setup), so it is non-zero. */
  static const uint8_t unlink_wire[48] = {0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x12, 0x34, 0x00, 0x01, 0x00, 0x01,
                                          0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x81, 0x00, 0x00, 0xab, 0xcd,
                                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc8, 0x00, 0x00, 0x00, 0x2a,
                                          0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x80, 0x06, 0x00, 0x01};
  uint32_t target = 0u;
  check(alk_usbip_parse_unlink(unlink_wire, sizeof(unlink_wire), &target) == ALK_USBIP_OK && target == 0xabcdu,
        "CMD_UNLINK with a stale tail parses to its target sequence");

  /* RET_SUBMIT for an isochronous IN transfer: 3 packets, one failed with -EXDEV (-18). */
  AlkUsbIpIsoPacket packets[3] = {{0u, 4u, 4u, 0}, {4u, 4u, 0u, -18}, {8u, 4u, 4u, 0}};
  AlkUsbIpCompletion completion = {sizeof(completion), ALK_USBIP_CORE_ABI_VERSION};
  completion.sequence = 0x1234u;
  completion.status = 0;
  completion.actual_length = 8u;
  completion.number_of_packets = 3u;
  completion.iso_packets = packets;
  completion.iso_packet_capacity = 3u;
  completion.iso_packet_count = 3u;
  completion.start_frame = 0x2au;
  const uint8_t payload[12] = {1, 2, 3, 4, 0, 0, 0, 0, 5, 6, 7, 8};
  uint8_t wire[48 + 12 + 48];
  size_t length = 0u;
  check(alk_usbip_build_ret_submit(&completion, payload, sizeof(payload), wire, sizeof(wire), &length) ==
                ALK_USBIP_OK &&
            length == sizeof(wire),
        "RET_SUBMIT with isochronous packets builds");
  check(be32(wire) == ALK_USBIP_RET_SUBMIT && be32(wire + 4u) == 0x1234u && be32(wire + 8u) == 0u &&
            be32(wire + 12u) == 0u && be32(wire + 16u) == 0u,
        "RET_SUBMIT basic header");
  check(be32(wire + 20u) == 0u && be32(wire + 24u) == 8u, "RET_SUBMIT status and actual length");
  check(be32(wire + 28u) == 0x2au, "RET_SUBMIT echoes start_frame");
  check(be32(wire + 32u) == 3u, "RET_SUBMIT number_of_packets is the descriptor count");
  check(be32(wire + 36u) == 1u, "RET_SUBMIT error_count counts the failed packet, not the packet count");
  check(be32(wire + 60u + 16u + 12u) == (uint32_t)-18, "second descriptor carries its status");
  /* A struct built before start_frame existed still builds and sends zero. */
  completion.struct_size = ALK_USBIP_COMPLETION_V1_BYTES;
  check(alk_usbip_build_ret_submit(&completion, payload, sizeof(payload), wire, sizeof(wire), &length) ==
                ALK_USBIP_OK &&
            be32(wire + 28u) == 0u && be32(wire + 36u) == 1u,
        "RET_SUBMIT from an older struct sends start_frame zero");
  {
    uint8_t unlink_reply[48];
    size_t unlink_length = 0u;
    check(alk_usbip_build_ret_unlink_ex(&completion, unlink_reply, sizeof(unlink_reply), &unlink_length) ==
                  ALK_USBIP_OK &&
              unlink_length == 48u,
          "an older completion struct is accepted by RET_UNLINK too");
  }
  /* No isochronous packets: both counters are zero. */
  completion = (AlkUsbIpCompletion){sizeof(completion), ALK_USBIP_CORE_ABI_VERSION};
  completion.sequence = 7u;
  check(alk_usbip_build_ret_submit(&completion, NULL, 0u, wire, sizeof(wire), &length) == ALK_USBIP_OK &&
            length == 48u && be32(wire + 32u) == 0u && be32(wire + 36u) == 0u,
        "RET_SUBMIT without packets has zero counts");
}

static AlkUsbIpError provider_unlink(void *context, uint32_t sequence, AlkUsbIpCompletion *completion) {
  (void)context;
  completion->sequence = sequence;
  return ALK_USBIP_OK;
}

static void test_listener_and_provider_schedule(void) {
  AlkUsbIpListenerCallbacks callbacks = {sizeof(callbacks), ALK_USBIP_CORE_ABI_VERSION, NULL, on_listener_event};
  AlkUsbIpListener *listener = NULL;
  check(alk_usbip_listener_create(&callbacks, &listener) == ALK_USBIP_OK, "create listener seam");
  check(alk_usbip_listener_notify(listener, ALK_USBIP_LISTENER_OPEN, NULL, NULL) == ALK_USBIP_OK &&
            (listener_events & (1u << ALK_USBIP_LISTENER_OPEN)) != 0u,
        "listener receives open event");
  alk_usbip_listener_destroy(listener);

  AlkUsbIpProviderSchedule schedule = {
      sizeof(schedule), ALK_USBIP_CORE_ABI_VERSION, 4u, NULL, provider_submit, provider_unlink, NULL};
  AlkUsbIpProviderScheduler *scheduler = NULL;
  check(alk_usbip_provider_scheduler_create(&schedule, &scheduler) == ALK_USBIP_OK, "create provider scheduler");
  AlkUsbIpUrb urb = {sizeof(urb), ALK_USBIP_CORE_ABI_VERSION};
  urb.sequence = 31u;
  AlkUsbIpCompletion completion = {sizeof(completion), ALK_USBIP_CORE_ABI_VERSION};
  check(alk_usbip_provider_scheduler_submit(scheduler, &urb) == ALK_USBIP_OK, "schedule provider submit");
  check(alk_usbip_provider_scheduler_complete(scheduler, urb.sequence, &completion) == ALK_USBIP_OK,
        "complete provider submit");
  alk_usbip_provider_scheduler_destroy(scheduler);
}

int main(void) {
  test_submit_and_unlink();
  test_real_sender_vectors();
  test_iso_and_audio_controls();
  test_listener_and_provider_schedule();
  if (failures != 0) {
    fprintf(stderr, "usbip_core_contract_test: %d failure(s)\n", failures);
    return 1;
  }
  puts("usbip_core_contract_test: all checks passed");
  return 0;
}
