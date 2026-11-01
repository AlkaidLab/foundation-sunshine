#ifndef ALKAIDLAB_USBIP_CORE_USBIP_CORE_H
#define ALKAIDLAB_USBIP_CORE_USBIP_CORE_H

/*
 * Bounded USB/IP 1.1.1 wire core.  The wire is big-endian and contains no
 * pointers.  Providers own USB descriptors and transfer buffers; this core
 * only validates framing and tracks submit/unlink ordering.  Source:
 * ds5-linux-usbip-protocol-20261031, USB/IP protocol sections; field widths
 * and reply sizes are cross-checked in the DS5 REPORT_LAYOUTS.md table.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALK_USBIP_CORE_ABI_VERSION 1u
#define ALK_USBIP_VERSION 0x0111u
#define ALK_USBIP_MAX_ISO_PACKETS 1024u
#define ALK_USBIP_MAX_TRANSFER_BYTES (1024u * 1024u)
#define ALK_USBIP_SUBMIT_HEADER_BYTES 48u
#define ALK_USBIP_ISO_DESCRIPTOR_BYTES 16u
#define ALK_USBIP_DS5_ISO_PACKET_NS 1000000ull

enum {
  ALK_USBIP_OP_REQ_DEVLIST = 0x8005u,
  ALK_USBIP_OP_REP_DEVLIST = 0x0005u,
  ALK_USBIP_OP_REQ_IMPORT = 0x8003u,
  ALK_USBIP_OP_REP_IMPORT = 0x0003u,
  ALK_USBIP_CMD_SUBMIT = 0x0001u,
  ALK_USBIP_CMD_UNLINK = 0x0002u,
  ALK_USBIP_RET_SUBMIT = 0x0003u,
  ALK_USBIP_RET_UNLINK = 0x0004u,
};

typedef enum AlkUsbIpError {
  ALK_USBIP_OK = 0,
  ALK_USBIP_ERROR_INVALID_ARGUMENT = 1,
  ALK_USBIP_ERROR_TRUNCATED = 2,
  ALK_USBIP_ERROR_BAD_VERSION = 3,
  ALK_USBIP_ERROR_UNKNOWN_OPCODE = 4,
  ALK_USBIP_ERROR_BAD_LENGTH = 5,
  ALK_USBIP_ERROR_LIMIT_EXCEEDED = 6,
  ALK_USBIP_ERROR_DUPLICATE_SEQUENCE = 7,
  ALK_USBIP_ERROR_NOT_FOUND = 8,
  ALK_USBIP_ERROR_RESET = 9,
} AlkUsbIpError;

typedef struct AlkUsbIpHeader {
  uint32_t struct_size;
  uint32_t abi;
  uint16_t version;
  uint16_t opcode;
  uint32_t status;
} AlkUsbIpHeader;

typedef struct AlkUsbIpUrb {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t sequence;
  uint32_t device_id;
  uint32_t direction;
  uint32_t endpoint;
  uint32_t transfer_flags;
  uint32_t transfer_buffer_length;
  uint32_t start_frame;
  uint32_t number_of_packets;
  uint32_t interval;
  uint8_t setup[8];
  struct AlkUsbIpIsoPacket *iso_packets;
  uint32_t iso_packet_capacity;
} AlkUsbIpUrb;

typedef struct AlkUsbIpIsoPacket {
  uint32_t offset;
  uint32_t length;
  uint32_t actual_length;
  int32_t status;
} AlkUsbIpIsoPacket;

typedef struct AlkUsbIpCompletion {
  uint32_t struct_size;
  uint32_t abi;
  int32_t status;
  uint32_t actual_length;
  uint32_t number_of_packets;
  bool submit_suppressed;
  uint32_t sequence;
  uint32_t device_id;
  uint32_t direction;
  uint32_t endpoint;
  AlkUsbIpIsoPacket *iso_packets;
  uint32_t iso_packet_capacity;
  uint32_t iso_packet_count;
  /* Appended: start_frame echoed in RET_SUBMIT. Callers whose struct_size ends before it send zero. */
  uint32_t start_frame;
} AlkUsbIpCompletion;

/* Size of AlkUsbIpCompletion before start_frame was appended; a caller that passes this struct_size is
 * accepted everywhere a completion is read or filled. */
#define ALK_USBIP_COMPLETION_V1_BYTES offsetof(AlkUsbIpCompletion, start_frame)

typedef struct AlkUsbIpTransport {
  uint32_t struct_size;
  uint32_t abi;
  void *context;
  AlkUsbIpError (*read)(void *, uint8_t *, size_t, size_t *);
  AlkUsbIpError (*write)(void *, const uint8_t *, size_t);
  void (*close)(void *);
} AlkUsbIpTransport;

typedef struct AlkUsbIpDeviceOps {
  uint32_t struct_size;
  uint32_t abi;
  void *context;
  AlkUsbIpError (*get_descriptor)(void *, uint8_t, uint8_t, uint16_t, uint8_t *, size_t, size_t *);
  AlkUsbIpError (*submit_urb)(void *, const AlkUsbIpUrb *, AlkUsbIpCompletion *);
  AlkUsbIpError (*unlink_urb)(void *, uint32_t);
  AlkUsbIpError (*reset)(void *);
  void (*release)(void *);
} AlkUsbIpDeviceOps;

typedef struct AlkUsbIpServerConfig {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t max_devices;
  uint32_t max_transfer_bytes;
  uint32_t iso_packet_limit;
} AlkUsbIpServerConfig;

typedef struct AlkUsbIpServer AlkUsbIpServer;

typedef struct AlkUsbIpServerApi {
  uint32_t struct_size;
  uint32_t abi;
  AlkUsbIpError (*create)(const AlkUsbIpServerConfig *, AlkUsbIpServer **);
  AlkUsbIpError (*add_device)(AlkUsbIpServer *, const char *, const AlkUsbIpDeviceOps *);
  AlkUsbIpError (*serve)(AlkUsbIpServer *, AlkUsbIpTransport *);
  void (*stop)(AlkUsbIpServer *);
  void (*destroy)(AlkUsbIpServer *);
} AlkUsbIpServerApi;

typedef struct AlkUsbIpUacFeatureUnit {
  uint32_t struct_size;
  uint32_t abi;
  uint16_t unit_id;
  uint8_t mute;
  uint8_t reserved;
  int16_t volume_current;
  int16_t volume_minimum;
  int16_t volume_maximum;
  int16_t volume_resolution;
} AlkUsbIpUacFeatureUnit;

typedef struct AlkUsbIpIsoPacer {
  uint32_t struct_size;
  uint32_t abi;
  uint64_t next_due_ns;
  uint32_t interval;
  uint32_t packet_count;
} AlkUsbIpIsoPacer;

typedef struct AlkUsbIpRequestTracker AlkUsbIpRequestTracker;

AlkUsbIpError alk_usbip_parse_header(const uint8_t *wire, size_t length, AlkUsbIpHeader *header);
AlkUsbIpError alk_usbip_parse_submit(const uint8_t *wire, size_t length, AlkUsbIpUrb *urb);
AlkUsbIpError alk_usbip_parse_unlink(const uint8_t *wire, size_t length, uint32_t *sequence);
AlkUsbIpError alk_usbip_request_tracker_create(AlkUsbIpRequestTracker **out_tracker);
void alk_usbip_request_tracker_destroy(AlkUsbIpRequestTracker *tracker);
AlkUsbIpError alk_usbip_request_tracker_submit(AlkUsbIpRequestTracker *tracker, const AlkUsbIpUrb *urb);
AlkUsbIpError alk_usbip_request_tracker_unlink(AlkUsbIpRequestTracker *tracker, uint32_t sequence,
                                               AlkUsbIpCompletion *completion);
bool alk_usbip_request_tracker_is_active(const AlkUsbIpRequestTracker *tracker, uint32_t sequence);
AlkUsbIpError alk_usbip_request_tracker_complete(AlkUsbIpRequestTracker *tracker, uint32_t sequence,
                                                 AlkUsbIpCompletion *completion);
AlkUsbIpError alk_usbip_build_ret_unlink(uint32_t sequence, int32_t status, uint8_t *wire, size_t capacity,
                                         size_t *length);
AlkUsbIpError alk_usbip_build_ret_unlink_ex(const AlkUsbIpCompletion *completion, uint8_t *wire, size_t capacity,
                                            size_t *length);
AlkUsbIpError alk_usbip_build_ret_submit(const AlkUsbIpCompletion *completion, const uint8_t *payload,
                                         size_t payload_length, uint8_t *wire, size_t capacity, size_t *length);
AlkUsbIpError alk_usbip_iso_pacer_init(AlkUsbIpIsoPacer *pacer, uint32_t interval);
AlkUsbIpError alk_usbip_iso_pacer_next(AlkUsbIpIsoPacer *pacer, uint64_t now_ns, uint32_t packet_count,
                                       uint64_t *due_ns);
AlkUsbIpError alk_usbip_uac_feature_get(const AlkUsbIpUacFeatureUnit *unit, uint8_t control, uint8_t request,
                                        int32_t *value);
AlkUsbIpError alk_usbip_uac_feature_set(AlkUsbIpUacFeatureUnit *unit, uint8_t control, int32_t value);

/* Append-only listener and provider scheduling seam. The transport remains
 * owned by the fork glue; these callbacks centralize lifecycle and URB
 * ordering without changing the frozen wire parser ABI. */
typedef enum AlkUsbIpListenerEvent {
  ALK_USBIP_LISTENER_OPEN = 1,
  ALK_USBIP_LISTENER_CLOSE = 2,
  ALK_USBIP_LISTENER_SUBMIT = 3,
  ALK_USBIP_LISTENER_UNLINK = 4,
} AlkUsbIpListenerEvent;

typedef struct AlkUsbIpListenerCallbacks {
  uint32_t struct_size;
  uint32_t abi;
  void *context;
  void (*on_event)(void *, AlkUsbIpListenerEvent, const AlkUsbIpUrb *, const AlkUsbIpCompletion *);
} AlkUsbIpListenerCallbacks;

typedef struct AlkUsbIpListener AlkUsbIpListener;

AlkUsbIpError alk_usbip_listener_create(const AlkUsbIpListenerCallbacks *callbacks, AlkUsbIpListener **out_listener);
void alk_usbip_listener_destroy(AlkUsbIpListener *listener);
AlkUsbIpError alk_usbip_listener_notify(AlkUsbIpListener *listener, AlkUsbIpListenerEvent event, const AlkUsbIpUrb *urb,
                                        const AlkUsbIpCompletion *completion);

typedef struct AlkUsbIpProviderSchedule {
  uint32_t struct_size;
  uint32_t abi;
  uint32_t max_inflight;
  void *context;
  AlkUsbIpError (*submit)(void *, const AlkUsbIpUrb *, AlkUsbIpCompletion *);
  AlkUsbIpError (*unlink)(void *, uint32_t, AlkUsbIpCompletion *);
  void (*on_complete)(void *, const AlkUsbIpCompletion *);
} AlkUsbIpProviderSchedule;

typedef struct AlkUsbIpProviderScheduler AlkUsbIpProviderScheduler;

AlkUsbIpError alk_usbip_provider_scheduler_create(const AlkUsbIpProviderSchedule *schedule,
                                                  AlkUsbIpProviderScheduler **out_scheduler);
void alk_usbip_provider_scheduler_destroy(AlkUsbIpProviderScheduler *scheduler);
AlkUsbIpError alk_usbip_provider_scheduler_submit(AlkUsbIpProviderScheduler *scheduler, const AlkUsbIpUrb *urb);
AlkUsbIpError alk_usbip_provider_scheduler_unlink(AlkUsbIpProviderScheduler *scheduler, uint32_t sequence,
                                                  AlkUsbIpCompletion *completion);
AlkUsbIpError alk_usbip_provider_scheduler_complete(AlkUsbIpProviderScheduler *scheduler, uint32_t sequence,
                                                    AlkUsbIpCompletion *completion);

#ifdef __cplusplus
}
#endif

#endif
