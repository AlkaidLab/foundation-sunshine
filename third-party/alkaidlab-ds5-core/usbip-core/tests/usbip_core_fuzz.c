#include "alkaidlab/usbip_core/usbip_core.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  AlkUsbIpIsoPacket packets[4];
  AlkUsbIpUrb urb = {sizeof(urb), ALK_USBIP_CORE_ABI_VERSION, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, {0}, packets, 4u};
  (void)alk_usbip_parse_submit(data, size, &urb);
  uint32_t sequence = 0u;
  (void)alk_usbip_parse_unlink(data, size, &sequence);
  AlkUsbIpHeader header = {sizeof(header), ALK_USBIP_CORE_ABI_VERSION};
  (void)alk_usbip_parse_header(data, size, &header);
  return 0;
}
