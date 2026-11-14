#include "alkaidlab/usbip_core/usbip_core.h"

#include <stdio.h>
#include <string.h>

static int failures;
static void check(int condition, const char *name) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", name);
    ++failures;
  }
}
static void put16(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)(value >> 8u);
  p[1] = (uint8_t)value;
}
static void put32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24u);
  p[1] = (uint8_t)(value >> 16u);
  p[2] = (uint8_t)(value >> 8u);
  p[3] = (uint8_t)value;
}

int main(void) {
  AlkUsbIpHeader header = {sizeof(header), ALK_USBIP_CORE_ABI_VERSION};
  uint8_t op[8] = {0};
  put16(op, 0x0110u);
  put16(op + 2u, ALK_USBIP_OP_REQ_DEVLIST);
  check(alk_usbip_parse_header(op, sizeof(op), &header) == ALK_USBIP_ERROR_BAD_VERSION,
        "wrong protocol version rejected");
  put16(op, ALK_USBIP_VERSION);
  put16(op + 2u, 0x1234u);
  check(alk_usbip_parse_header(op, sizeof(op), &header) == ALK_USBIP_ERROR_UNKNOWN_OPCODE,
        "unknown operation rejected");
  check(alk_usbip_parse_header(op, 7u, &header) == ALK_USBIP_ERROR_TRUNCATED, "truncated operation rejected");

  uint8_t submit[48] = {0};
  put32(submit, ALK_USBIP_CMD_SUBMIT);
  put32(submit + 24u, ALK_USBIP_MAX_TRANSFER_BYTES + 1u);
  AlkUsbIpUrb urb = {sizeof(urb), ALK_USBIP_CORE_ABI_VERSION};
  check(alk_usbip_parse_submit(submit, sizeof(submit), &urb) == ALK_USBIP_ERROR_LIMIT_EXCEEDED,
        "oversized submit rejected");
  put32(submit + 24u, 0u);
  put32(submit + 32u, ALK_USBIP_MAX_ISO_PACKETS + 1u);
  check(alk_usbip_parse_submit(submit, sizeof(submit), &urb) == ALK_USBIP_ERROR_LIMIT_EXCEEDED,
        "excessive ISO packet count rejected");

  uint8_t unlink[48] = {0};
  put32(unlink, ALK_USBIP_CMD_UNLINK);
  put32(unlink + 20u, 9u);
  uint32_t sequence = 0u;
  check(alk_usbip_parse_unlink(unlink, sizeof(unlink), &sequence) == ALK_USBIP_OK && sequence == 9u,
        "unlink sequence vector");
  /* The tail after unlink_seqnum is unused union space; usbip-win2 leaves stale submit fields there. */
  unlink[24] = 1u;
  check(alk_usbip_parse_unlink(unlink, sizeof(unlink), &sequence) == ALK_USBIP_OK && sequence == 9u,
        "unlink with nonzero tail accepted");
  check(alk_usbip_parse_unlink(unlink, sizeof(unlink) - 1u, &sequence) == ALK_USBIP_ERROR_TRUNCATED,
        "short unlink rejected");
  unlink[3] = ALK_USBIP_CMD_SUBMIT;
  check(alk_usbip_parse_unlink(unlink, sizeof(unlink), &sequence) == ALK_USBIP_ERROR_UNKNOWN_OPCODE,
        "unlink with another command rejected");
  if (failures != 0) {
    fprintf(stderr, "usbip_core_negative_test: %d failure(s)\n", failures);
    return 1;
  }
  puts("usbip_core_negative_test: all checks passed");
  return 0;
}
