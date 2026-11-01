#include "alkaidlab/usbip_core/usbip_core.h"

#include <stdlib.h>
#include <string.h>

enum { USBIP_OP_HEADER_BYTES = 8u, USBIP_SUBMIT_BYTES = 48u, USBIP_UNLINK_BYTES = 48u, TRACKER_SLOTS = 128u };

typedef struct TrackerEntry {
  bool active;
  uint32_t sequence;
  uint32_t device_id;
  uint32_t direction;
  uint32_t endpoint;
} TrackerEntry;

struct AlkUsbIpRequestTracker {
  TrackerEntry entries[TRACKER_SLOTS];
};

static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8u) | p[1]); }
static uint32_t get_u32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24u) | ((uint32_t)p[1] << 16u) | ((uint32_t)p[2] << 8u) | p[3];
}
static void put_u32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24u);
  p[1] = (uint8_t)(value >> 16u);
  p[2] = (uint8_t)(value >> 8u);
  p[3] = (uint8_t)value;
}
static void put_i32(uint8_t *p, int32_t value) { put_u32(p, (uint32_t)value); }

static AlkUsbIpError parse_common(const uint8_t *wire, size_t length, uint16_t *opcode) {
  if (wire == NULL || opcode == NULL) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  if (length < USBIP_OP_HEADER_BYTES) {
    return ALK_USBIP_ERROR_TRUNCATED;
  }
  if (get_u16(wire) != ALK_USBIP_VERSION) {
    return ALK_USBIP_ERROR_BAD_VERSION;
  }
  *opcode = get_u16(wire + 2u);
  return ALK_USBIP_OK;
}

AlkUsbIpError alk_usbip_parse_header(const uint8_t *wire, size_t length, AlkUsbIpHeader *header) {
  if (header == NULL || header->struct_size < offsetof(AlkUsbIpHeader, abi) + sizeof(header->abi) ||
      header->abi != ALK_USBIP_CORE_ABI_VERSION || header->struct_size < sizeof(*header)) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  uint16_t opcode = 0u;
  AlkUsbIpError result = parse_common(wire, length, &opcode);
  if (result != ALK_USBIP_OK) {
    return result;
  }
  switch (opcode) {
  case ALK_USBIP_OP_REQ_DEVLIST:
  case ALK_USBIP_OP_REP_DEVLIST:
  case ALK_USBIP_OP_REQ_IMPORT:
  case ALK_USBIP_OP_REP_IMPORT:
    break;
  default:
    return ALK_USBIP_ERROR_UNKNOWN_OPCODE;
  }
  header->version = ALK_USBIP_VERSION;
  header->opcode = opcode;
  header->status = get_u32(wire + 4u);
  return ALK_USBIP_OK;
}

AlkUsbIpError alk_usbip_parse_submit(const uint8_t *wire, size_t length, AlkUsbIpUrb *urb) {
  if (wire == NULL || urb == NULL || urb->struct_size < offsetof(AlkUsbIpUrb, abi) + sizeof(urb->abi) ||
      urb->abi != ALK_USBIP_CORE_ABI_VERSION || urb->struct_size < offsetof(AlkUsbIpUrb, setup) + sizeof(urb->setup)) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  if (length < USBIP_SUBMIT_BYTES) {
    return ALK_USBIP_ERROR_TRUNCATED;
  }
  if (get_u32(wire) != ALK_USBIP_CMD_SUBMIT) {
    return ALK_USBIP_ERROR_UNKNOWN_OPCODE;
  }
  const uint32_t transfer_length = get_u32(wire + 24u);
  const uint32_t packets = get_u32(wire + 32u);
  AlkUsbIpIsoPacket *iso_packets = NULL;
  uint32_t iso_capacity = 0u;
  if (urb->struct_size >= offsetof(AlkUsbIpUrb, iso_packet_capacity) + sizeof(urb->iso_packet_capacity)) {
    iso_packets = urb->iso_packets;
    iso_capacity = urb->iso_packet_capacity;
  }
  if (transfer_length > ALK_USBIP_MAX_TRANSFER_BYTES ||
      (packets != UINT32_MAX && packets > ALK_USBIP_MAX_ISO_PACKETS)) {
    return ALK_USBIP_ERROR_LIMIT_EXCEEDED;
  }
  if (get_u32(wire + 12u) > 1u || get_u32(wire + 16u) > 0x8fu || (get_u32(wire + 16u) & 0x70u) != 0u ||
      (packets != UINT32_MAX && packets > 0u && (iso_packets == NULL || iso_capacity < packets))) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  const uint32_t iso_count = packets == UINT32_MAX ? 0u : packets;
  const size_t payload_offset = ALK_USBIP_SUBMIT_HEADER_BYTES + (get_u32(wire + 12u) == 0u ? transfer_length : 0u);
  const size_t descriptor_bytes = (size_t)iso_count * ALK_USBIP_ISO_DESCRIPTOR_BYTES;
  if (length < payload_offset + descriptor_bytes) {
    return ALK_USBIP_ERROR_TRUNCATED;
  }
  AlkUsbIpUrb parsed;
  memset(&parsed, 0, sizeof(parsed));
  parsed.struct_size = sizeof(parsed);
  parsed.abi = ALK_USBIP_CORE_ABI_VERSION;
  parsed.sequence = get_u32(wire + 4u);
  parsed.device_id = get_u32(wire + 8u);
  parsed.direction = get_u32(wire + 12u);
  parsed.endpoint = get_u32(wire + 16u);
  parsed.transfer_flags = get_u32(wire + 20u);
  parsed.transfer_buffer_length = transfer_length;
  parsed.start_frame = get_u32(wire + 28u);
  parsed.number_of_packets = packets;
  parsed.interval = get_u32(wire + 36u);
  memcpy(parsed.setup, wire + 40u, sizeof(parsed.setup));
  parsed.iso_packets = iso_packets;
  parsed.iso_packet_capacity = iso_capacity;
  for (uint32_t index = 0u; index < iso_count; ++index) {
    const uint8_t *descriptor = wire + payload_offset + index * ALK_USBIP_ISO_DESCRIPTOR_BYTES;
    iso_packets[index].offset = get_u32(descriptor);
    iso_packets[index].length = get_u32(descriptor + 4u);
    iso_packets[index].actual_length = get_u32(descriptor + 8u);
    iso_packets[index].status = (int32_t)get_u32(descriptor + 12u);
  }
  const size_t copy_size = urb->struct_size < sizeof(parsed) ? urb->struct_size : sizeof(parsed);
  memcpy(urb, &parsed, copy_size);
  if (iso_count != 0u) {
    uint64_t total = 0u;
    for (uint32_t index = 0u; index < iso_count; ++index) {
      total += iso_packets[index].length;
      if ((uint64_t)iso_packets[index].offset + iso_packets[index].length > transfer_length) {
        return ALK_USBIP_ERROR_BAD_LENGTH;
      }
    }
    if (total != transfer_length) {
      return ALK_USBIP_ERROR_BAD_LENGTH;
    }
  }
  return ALK_USBIP_OK;
}

AlkUsbIpError alk_usbip_parse_unlink(const uint8_t *wire, size_t length, uint32_t *sequence) {
  if (wire == NULL || sequence == NULL) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  if (length < USBIP_UNLINK_BYTES) {
    return ALK_USBIP_ERROR_TRUNCATED;
  }
  if (get_u32(wire) != ALK_USBIP_CMD_UNLINK) {
    return ALK_USBIP_ERROR_UNKNOWN_OPCODE;
  }
  /* Only unlink_seqnum (offset 20) is defined for CMD_UNLINK. The 24 bytes after it are the unused tail
   * of the header union: the Linux kernel zeroes them, usbip-win2 sends whatever its header buffer held
   * (stale submit fields), so their content carries no meaning and is not checked. */
  *sequence = get_u32(wire + 20u);
  return ALK_USBIP_OK;
}

AlkUsbIpError alk_usbip_request_tracker_create(AlkUsbIpRequestTracker **out_tracker) {
  if (out_tracker == NULL) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  *out_tracker = (AlkUsbIpRequestTracker *)calloc(1u, sizeof(**out_tracker));
  return *out_tracker == NULL ? ALK_USBIP_ERROR_LIMIT_EXCEEDED : ALK_USBIP_OK;
}

void alk_usbip_request_tracker_destroy(AlkUsbIpRequestTracker *tracker) { free(tracker); }

static TrackerEntry *find_entry(AlkUsbIpRequestTracker *tracker, uint32_t sequence) {
  if (tracker == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < TRACKER_SLOTS; ++i) {
    if (tracker->entries[i].active && tracker->entries[i].sequence == sequence) {
      return &tracker->entries[i];
    }
  }
  return NULL;
}

AlkUsbIpError alk_usbip_request_tracker_submit(AlkUsbIpRequestTracker *tracker, const AlkUsbIpUrb *urb) {
  if (tracker == NULL || urb == NULL || urb->struct_size < offsetof(AlkUsbIpUrb, abi) + sizeof(urb->abi) ||
      urb->abi != ALK_USBIP_CORE_ABI_VERSION || urb->struct_size < sizeof(*urb)) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  if (find_entry(tracker, urb->sequence) != NULL) {
    return ALK_USBIP_ERROR_DUPLICATE_SEQUENCE;
  }
  for (size_t i = 0; i < TRACKER_SLOTS; ++i) {
    if (!tracker->entries[i].active) {
      tracker->entries[i].active = true;
      tracker->entries[i].sequence = urb->sequence;
      tracker->entries[i].device_id = urb->device_id;
      tracker->entries[i].direction = urb->direction;
      tracker->entries[i].endpoint = urb->endpoint;
      return ALK_USBIP_OK;
    }
  }
  return ALK_USBIP_ERROR_LIMIT_EXCEEDED;
}

AlkUsbIpError alk_usbip_request_tracker_unlink(AlkUsbIpRequestTracker *tracker, uint32_t sequence,
                                               AlkUsbIpCompletion *completion) {
  if (tracker == NULL || completion == NULL ||
      completion->struct_size < offsetof(AlkUsbIpCompletion, abi) + sizeof(completion->abi) ||
      completion->abi != ALK_USBIP_CORE_ABI_VERSION || completion->struct_size < ALK_USBIP_COMPLETION_V1_BYTES) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  TrackerEntry *entry = find_entry(tracker, sequence);
  if (entry == NULL) {
    return ALK_USBIP_ERROR_NOT_FOUND;
  }
  entry->active = false;
  completion->sequence = sequence;
  completion->device_id = entry->device_id;
  completion->direction = entry->direction;
  completion->endpoint = entry->endpoint;
  completion->status = -104; /* -ECONNRESET, required by USB/IP CMD_UNLINK semantics. */
  completion->actual_length = 0u;
  completion->number_of_packets = 0u;
  completion->submit_suppressed = true;
  return ALK_USBIP_OK;
}

bool alk_usbip_request_tracker_is_active(const AlkUsbIpRequestTracker *tracker, uint32_t sequence) {
  if (tracker == NULL) {
    return false;
  }
  for (size_t i = 0; i < TRACKER_SLOTS; ++i) {
    if (tracker->entries[i].active && tracker->entries[i].sequence == sequence) {
      return true;
    }
  }
  return false;
}

AlkUsbIpError alk_usbip_request_tracker_complete(AlkUsbIpRequestTracker *tracker, uint32_t sequence,
                                                 AlkUsbIpCompletion *completion) {
  if (tracker == NULL || completion == NULL ||
      completion->struct_size < offsetof(AlkUsbIpCompletion, abi) + sizeof(completion->abi) ||
      completion->abi != ALK_USBIP_CORE_ABI_VERSION || completion->struct_size < ALK_USBIP_COMPLETION_V1_BYTES) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  TrackerEntry *entry = find_entry(tracker, sequence);
  if (entry == NULL) {
    return ALK_USBIP_ERROR_NOT_FOUND;
  }
  entry->active = false;
  completion->sequence = sequence;
  completion->device_id = entry->device_id;
  completion->direction = entry->direction;
  completion->endpoint = entry->endpoint;
  completion->submit_suppressed = false;
  return ALK_USBIP_OK;
}

AlkUsbIpError alk_usbip_build_ret_unlink(uint32_t sequence, int32_t status, uint8_t *wire, size_t capacity,
                                         size_t *length) {
  if (wire == NULL || length == NULL) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  if (capacity < 48u) {
    return ALK_USBIP_ERROR_TRUNCATED;
  }
  memset(wire, 0, 48u);
  put_u32(wire, ALK_USBIP_RET_UNLINK);
  put_u32(wire + 4u, sequence);
  /* device_id, direction, and endpoint are carried by the caller's completion
   * context in the submit reply; unlink packets echo zero for these fields. */
  put_i32(wire + 20u, status);
  *length = 48u;
  return ALK_USBIP_OK;
}

AlkUsbIpError alk_usbip_build_ret_unlink_ex(const AlkUsbIpCompletion *completion, uint8_t *wire, size_t capacity,
                                            size_t *length) {
  if (completion == NULL || completion->struct_size < offsetof(AlkUsbIpCompletion, abi) + sizeof(completion->abi) ||
      completion->abi != ALK_USBIP_CORE_ABI_VERSION || completion->struct_size < ALK_USBIP_COMPLETION_V1_BYTES) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  if (wire == NULL || length == NULL || capacity < 48u) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  memset(wire, 0, 48u);
  put_u32(wire, ALK_USBIP_RET_UNLINK);
  put_u32(wire + 4u, completion->sequence);
  /* Server replies zero devid, direction and endpoint per usbip_header_basic. */
  put_u32(wire + 8u, 0u);
  put_u32(wire + 12u, 0u);
  put_u32(wire + 16u, 0u);
  put_i32(wire + 20u, completion->status);
  *length = 48u;
  return ALK_USBIP_OK;
}

AlkUsbIpError alk_usbip_build_ret_submit(const AlkUsbIpCompletion *completion, const uint8_t *payload,
                                         size_t payload_length, uint8_t *wire, size_t capacity, size_t *length) {
  if (completion == NULL || wire == NULL || length == NULL ||
      completion->struct_size < offsetof(AlkUsbIpCompletion, abi) + sizeof(completion->abi) ||
      completion->abi != ALK_USBIP_CORE_ABI_VERSION || completion->struct_size < ALK_USBIP_COMPLETION_V1_BYTES ||
      (payload == NULL && payload_length != 0u)) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  if (payload_length > ALK_USBIP_MAX_TRANSFER_BYTES || completion->iso_packet_count > ALK_USBIP_MAX_ISO_PACKETS ||
      (completion->iso_packet_count != 0u &&
       (completion->iso_packets == NULL || completion->iso_packet_capacity < completion->iso_packet_count)) ||
      capacity < 48u + payload_length + (size_t)completion->iso_packet_count * ALK_USBIP_ISO_DESCRIPTOR_BYTES) {
    return ALK_USBIP_ERROR_TRUNCATED;
  }
  const size_t descriptor_offset = 48u + payload_length;
  const size_t total_length = descriptor_offset + (size_t)completion->iso_packet_count * ALK_USBIP_ISO_DESCRIPTOR_BYTES;
  memset(wire, 0, total_length);
  put_u32(wire, ALK_USBIP_RET_SUBMIT);
  put_u32(wire + 4u, completion->sequence);
  /* setup_base_pdu() zeros these three server reply fields. */
  put_u32(wire + 8u, 0u);
  put_u32(wire + 12u, 0u);
  put_u32(wire + 16u, 0u);
  put_i32(wire + 20u, completion->status);
  put_u32(wire + 24u, completion->actual_length);
  /* start_frame echoes the request (appended field, zero for a caller built before it existed);
   * number_of_packets is the descriptor count; error_count is the number of isochronous packets that
   * completed with a non-zero status, as the Linux usbip_pack_ret_submit() carries urb->error_count. */
  const uint32_t start_frame = completion->struct_size >= offsetof(AlkUsbIpCompletion, start_frame) + sizeof(uint32_t)
                                   ? completion->start_frame
                                   : 0u;
  uint32_t error_count = 0u;
  for (uint32_t index = 0u; index < completion->iso_packet_count; ++index) {
    if (completion->iso_packets[index].status != 0) {
      ++error_count;
    }
  }
  put_u32(wire + 28u, start_frame);
  put_u32(wire + 32u, completion->number_of_packets);
  put_u32(wire + 36u, error_count);
  if (payload_length != 0u) {
    memcpy(wire + 48u, payload, payload_length);
  }
  for (uint32_t index = 0u; index < completion->iso_packet_count; ++index) {
    const AlkUsbIpIsoPacket *packet = &completion->iso_packets[index];
    uint8_t *descriptor = wire + descriptor_offset + index * ALK_USBIP_ISO_DESCRIPTOR_BYTES;
    put_u32(descriptor, packet->offset);
    put_u32(descriptor + 4u, packet->length);
    put_u32(descriptor + 8u, packet->actual_length);
    put_i32(descriptor + 12u, packet->status);
  }
  *length = total_length;
  return ALK_USBIP_OK;
}

AlkUsbIpError alk_usbip_iso_pacer_init(AlkUsbIpIsoPacer *pacer, uint32_t interval) {
  if (pacer == NULL || interval == 0u || pacer->struct_size < offsetof(AlkUsbIpIsoPacer, abi) + sizeof(pacer->abi) ||
      pacer->abi != ALK_USBIP_CORE_ABI_VERSION || pacer->struct_size < sizeof(*pacer)) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  memset(pacer, 0, sizeof(*pacer));
  pacer->struct_size = sizeof(*pacer);
  pacer->abi = ALK_USBIP_CORE_ABI_VERSION;
  pacer->interval = interval;
  return ALK_USBIP_OK;
}

AlkUsbIpError alk_usbip_iso_pacer_next(AlkUsbIpIsoPacer *pacer, uint64_t now_ns, uint32_t packet_count,
                                       uint64_t *due_ns) {
  if (pacer == NULL || due_ns == NULL || pacer->struct_size < offsetof(AlkUsbIpIsoPacer, abi) + sizeof(pacer->abi) ||
      pacer->abi != ALK_USBIP_CORE_ABI_VERSION || pacer->struct_size < sizeof(*pacer) || packet_count == 0u) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  const uint64_t start = pacer->next_due_ns > now_ns ? pacer->next_due_ns : now_ns;
  *due_ns = start;
  pacer->next_due_ns = start + (uint64_t)packet_count * ALK_USBIP_DS5_ISO_PACKET_NS;
  pacer->packet_count += packet_count;
  return ALK_USBIP_OK;
}

AlkUsbIpError alk_usbip_uac_feature_get(const AlkUsbIpUacFeatureUnit *unit, uint8_t control, uint8_t request,
                                        int32_t *value) {
  if (unit == NULL || value == NULL || unit->struct_size < offsetof(AlkUsbIpUacFeatureUnit, abi) + sizeof(unit->abi) ||
      unit->abi != ALK_USBIP_CORE_ABI_VERSION || unit->struct_size < sizeof(*unit)) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  if (control == 1u && request == 0x81u) {
    *value = unit->mute;
  } else if (control == 2u && request == 0x81u) {
    *value = unit->volume_current;
  } else if (control == 2u && request == 0x82u) {
    *value = unit->volume_minimum;
  } else if (control == 2u && request == 0x83u) {
    *value = unit->volume_maximum;
  } else if (control == 2u && request == 0x84u) {
    *value = unit->volume_resolution;
  } else {
    return ALK_USBIP_ERROR_NOT_FOUND;
  }
  return ALK_USBIP_OK;
}

AlkUsbIpError alk_usbip_uac_feature_set(AlkUsbIpUacFeatureUnit *unit, uint8_t control, int32_t value) {
  if (unit == NULL || unit->struct_size < offsetof(AlkUsbIpUacFeatureUnit, abi) + sizeof(unit->abi) ||
      unit->abi != ALK_USBIP_CORE_ABI_VERSION || unit->struct_size < sizeof(*unit)) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  if (control == 1u && (value == 0 || value == 1)) {
    unit->mute = (uint8_t)value;
  } else if (control == 2u && unit->volume_resolution != 0 && value >= unit->volume_minimum &&
             value <= unit->volume_maximum && ((value - unit->volume_minimum) % unit->volume_resolution) == 0) {
    unit->volume_current = (int16_t)value;
  } else {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  return ALK_USBIP_OK;
}

struct AlkUsbIpListener {
  AlkUsbIpListenerCallbacks callbacks;
};

struct AlkUsbIpProviderScheduler {
  AlkUsbIpProviderSchedule schedule;
  AlkUsbIpRequestTracker *tracker;
  uint32_t inflight;
};

AlkUsbIpError alk_usbip_listener_create(const AlkUsbIpListenerCallbacks *callbacks, AlkUsbIpListener **out_listener) {
  if (callbacks == NULL || out_listener == NULL || callbacks->struct_size < sizeof(*callbacks) ||
      callbacks->abi != ALK_USBIP_CORE_ABI_VERSION || callbacks->on_event == NULL) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  AlkUsbIpListener *listener = (AlkUsbIpListener *)calloc(1u, sizeof(*listener));
  if (listener == NULL)
    return ALK_USBIP_ERROR_LIMIT_EXCEEDED;
  listener->callbacks = *callbacks;
  *out_listener = listener;
  return ALK_USBIP_OK;
}

void alk_usbip_listener_destroy(AlkUsbIpListener *listener) { free(listener); }

AlkUsbIpError alk_usbip_listener_notify(AlkUsbIpListener *listener, AlkUsbIpListenerEvent event, const AlkUsbIpUrb *urb,
                                        const AlkUsbIpCompletion *completion) {
  if (listener == NULL || listener->callbacks.on_event == NULL || event < ALK_USBIP_LISTENER_OPEN ||
      event > ALK_USBIP_LISTENER_UNLINK) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  listener->callbacks.on_event(listener->callbacks.context, event, urb, completion);
  return ALK_USBIP_OK;
}

AlkUsbIpError alk_usbip_provider_scheduler_create(const AlkUsbIpProviderSchedule *schedule,
                                                  AlkUsbIpProviderScheduler **out_scheduler) {
  if (schedule == NULL || out_scheduler == NULL || schedule->struct_size < sizeof(*schedule) ||
      schedule->abi != ALK_USBIP_CORE_ABI_VERSION || schedule->max_inflight == 0u || schedule->submit == NULL ||
      schedule->unlink == NULL) {
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  }
  AlkUsbIpProviderScheduler *scheduler = (AlkUsbIpProviderScheduler *)calloc(1u, sizeof(*scheduler));
  if (scheduler == NULL)
    return ALK_USBIP_ERROR_LIMIT_EXCEEDED;
  scheduler->schedule = *schedule;
  AlkUsbIpError result = alk_usbip_request_tracker_create(&scheduler->tracker);
  if (result != ALK_USBIP_OK) {
    free(scheduler);
    return result;
  }
  *out_scheduler = scheduler;
  return ALK_USBIP_OK;
}

void alk_usbip_provider_scheduler_destroy(AlkUsbIpProviderScheduler *scheduler) {
  if (scheduler == NULL)
    return;
  alk_usbip_request_tracker_destroy(scheduler->tracker);
  free(scheduler);
}

AlkUsbIpError alk_usbip_provider_scheduler_submit(AlkUsbIpProviderScheduler *scheduler, const AlkUsbIpUrb *urb) {
  if (scheduler == NULL || urb == NULL || scheduler->inflight >= scheduler->schedule.max_inflight) {
    return ALK_USBIP_ERROR_LIMIT_EXCEEDED;
  }
  AlkUsbIpError result = alk_usbip_request_tracker_submit(scheduler->tracker, urb);
  if (result != ALK_USBIP_OK)
    return result;
  AlkUsbIpCompletion completion = {sizeof(completion), ALK_USBIP_CORE_ABI_VERSION};
  result = scheduler->schedule.submit(scheduler->schedule.context, urb, &completion);
  if (result != ALK_USBIP_OK) {
    (void)alk_usbip_request_tracker_complete(scheduler->tracker, urb->sequence, &completion);
    return result;
  }
  ++scheduler->inflight;
  if (scheduler->schedule.on_complete != NULL)
    scheduler->schedule.on_complete(scheduler->schedule.context, &completion);
  return ALK_USBIP_OK;
}

AlkUsbIpError alk_usbip_provider_scheduler_unlink(AlkUsbIpProviderScheduler *scheduler, uint32_t sequence,
                                                  AlkUsbIpCompletion *completion) {
  if (scheduler == NULL || completion == NULL)
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  AlkUsbIpError result = scheduler->schedule.unlink(scheduler->schedule.context, sequence, completion);
  if (result != ALK_USBIP_OK)
    return result;
  result = alk_usbip_request_tracker_unlink(scheduler->tracker, sequence, completion);
  if (result == ALK_USBIP_OK && scheduler->inflight != 0u)
    --scheduler->inflight;
  if (result == ALK_USBIP_OK && scheduler->schedule.on_complete != NULL)
    scheduler->schedule.on_complete(scheduler->schedule.context, completion);
  return result;
}

AlkUsbIpError alk_usbip_provider_scheduler_complete(AlkUsbIpProviderScheduler *scheduler, uint32_t sequence,
                                                    AlkUsbIpCompletion *completion) {
  if (scheduler == NULL || completion == NULL)
    return ALK_USBIP_ERROR_INVALID_ARGUMENT;
  AlkUsbIpError result = alk_usbip_request_tracker_complete(scheduler->tracker, sequence, completion);
  if (result == ALK_USBIP_OK && scheduler->inflight != 0u)
    --scheduler->inflight;
  if (result == ALK_USBIP_OK && scheduler->schedule.on_complete != NULL)
    scheduler->schedule.on_complete(scheduler->schedule.context, completion);
  return result;
}
