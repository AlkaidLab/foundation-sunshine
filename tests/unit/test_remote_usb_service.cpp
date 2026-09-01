#include <gtest/gtest.h>

#include "src/remote_usb/remote_usb_service.h"

TEST(RemoteUsbService, StartsInUnavailableState) {
  remote_usb::remote_usb_service service;
  EXPECT_FALSE(service.available());
  EXPECT_EQ(service.bound_port(), 0);
  service.stop();
  service.stop();
}

TEST(RemoteUsbService, RejectsCapabilityBeforeStart) {
  remote_usb::remote_usb_service service;
  remote_usb::capability_issue_request request;
  request.client_uuid = "client";
  request.stream_generation = 1;
  request.endpoint_host = "127.0.0.1";
  request.wire_client_uuid = "0123456789ABCDEF";
  request.session_token = 1;
  request.attachment_token = 2;
  request.lease_token = 3;
  const auto result = service.issue_capability(std::move(request));
  EXPECT_EQ(result.status, remote_usb::capability_issue_status::unavailable);
  EXPECT_FALSE(result);
}
