/** @file src/remote_usb/usbip_endpoint.h
 *  @brief Value-only endpoint shared by local USB/IP consumers.
 */
#pragma once

#include <cstdint>
#include <string>

namespace remote_usb {

struct endpoint {
  std::string address { "127.0.0.1" };
  std::uint16_t port { 0 };
  std::string busid;

  friend bool operator==(const endpoint &, const endpoint &) = default;
};

}  // namespace remote_usb
