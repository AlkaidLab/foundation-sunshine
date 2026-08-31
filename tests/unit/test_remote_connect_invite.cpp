#include <gtest/gtest.h>

#include <stdexcept>

#include "src/remote_connect/invite.h"

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

  TEST(RemoteConnectInvite, BuildsVersionedRemoteEnrollment) {
    const auto uri = remote_connect::build_invite({
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
    });

    EXPECT_EQ(
      uri,
      "moonlight://pair?v=2&host=10.80.42.1&port=47989&pin=9876&name=PC%20%26%20TV"
      "&profile=host%2Fprofile&et_host=10.80.42.1&et_name=remote%20network"
      "&et_secret=secret%2Bvalue&et_peer=udp%3A%2F%2Fpublic.easytier.top%3A11010"
      "&expires=1800000000"
    );
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

}  // namespace
