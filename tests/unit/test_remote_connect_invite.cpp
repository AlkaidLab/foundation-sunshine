#include <gtest/gtest.h>

#include <stdexcept>

#include "src/remote_connect/invite.h"
#include "src/remote_connect/pairing.h"
#include "src/remote_connect/service.h"

namespace {
  remote_connect::status_t fake_status;
}

namespace remote_connect {
  status_t
  status() {
    return fake_status;
  }
}

namespace {

  TEST(RemoteConnectInvite, PreservesLegacyLanFormat) {
    const auto uri = remote_connect::build_invite({
      "192.168.1.20",
      47989,
      "0123",
      "Living Room PC",
      std::nullopt,
      0,
    });

    EXPECT_EQ(
      uri,
      "moonlight://pair?host=192.168.1.20&port=47989&pin=0123&name=Living%20Room%20PC"
    );
  }

  TEST(RemoteConnectInvite, RefusesLegacyRemoteSecretInUri) {
    const remote_connect::invite_t invite {
      "10.80.42.1",
      47989,
      "9876",
      "PC & TV",
      remote_connect::enrollment_t {
        "host/profile",
        "10.80.42.1",
        "remote network",
        "secret+value",
        "udp://public.easytier.top:11010",
      },
      1'800'000'000,
    };
    EXPECT_THROW(remote_connect::build_invite(invite), std::invalid_argument);
  }

  TEST(RemoteConnectInvite, RejectsIncompleteRemoteEnrollment) {
    remote_connect::invite_t invite {
      "10.80.42.1",
      47989,
      "9876",
      "PC",
      remote_connect::enrollment_t {},
      1'800'000'000,
    };

    EXPECT_THROW(remote_connect::build_invite(invite), std::invalid_argument);
  }

  TEST(RemoteConnectInvite, RemoteEnabledPairingFailsClosedWithoutReturningSecretOrPin) {
    fake_status = {true, true, true, "10.80.42.1", {}};
    const auto result = remote_connect::create_pairing_invite({
      "192.168.1.20", 47989, "9876", "PC", 1'800'000'000, 47989
    });
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.remote);
    EXPECT_TRUE(result.host.empty());
    EXPECT_TRUE(result.url.empty());
    EXPECT_EQ(result.error, "Secure remote enrollment is not available yet");
    EXPECT_EQ(result.error.find("permanent-secret"), std::string::npos);
    EXPECT_EQ(result.error.find("9876"), std::string::npos);
  }

  TEST(RemoteConnectInvite, RemoteUnavailableStillReportsSecureEnrollmentUnavailable) {
    fake_status = {true, false, false, {}, "EasyTier is not running"};
    const auto result = remote_connect::create_pairing_invite({
      "192.168.1.20", 47989, "9876", "PC", 1'800'000'000, 47989
    });
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.remote);
    EXPECT_TRUE(result.url.empty());
    EXPECT_EQ(result.error, "Secure remote enrollment is not available yet");
  }

  TEST(RemoteConnectInvite, LocalPairingStillProducesLegacyUri) {
    fake_status = {false, false, true, {}, {}};
    const auto result = remote_connect::create_pairing_invite({
      "192.168.1.20", 47989, "0123", "Living Room PC", 0, 0
    });
    ASSERT_TRUE(result.success);
    EXPECT_FALSE(result.remote);
    EXPECT_EQ(result.url, "moonlight://pair?host=192.168.1.20&port=47989&pin=0123&name=Living%20Room%20PC");
  }

}  // namespace
