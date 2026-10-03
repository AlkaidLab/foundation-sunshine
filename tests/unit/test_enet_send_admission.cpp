#include <enet/enet.h>

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {
  struct admission_t {
    bool deny = false;
    unsigned admitted = 0;
    unsigned completed = 0;
    unsigned attempted = 0;
    unsigned zero = 0;
    std::size_t payload_bytes = 0;
    std::size_t maximum = 0;

    static int ENET_CALLBACK
    begin(void *raw, ENetPeer *, std::size_t maximum) {
      auto &state = *static_cast<admission_t *>(raw);
      if (state.deny) return 0;
      ++state.admitted;
      state.maximum = maximum;
      return 1;
    }
    static void ENET_CALLBACK
    finish(void *raw, ENetPeer *, std::size_t bytes, int sent, int attempted) {
      auto &state = *static_cast<admission_t *>(raw);
      ++state.completed;
      if (attempted) {
        ++state.attempted;
        EXPECT_EQ(sent, static_cast<int>(bytes));
        EXPECT_LE(bytes, state.maximum);
        state.payload_bytes += bytes;
      }
      else {
        ++state.zero;
        EXPECT_EQ(bytes, 0U);
      }
    }
  };

  class EnetAdmission: public testing::Test {
  protected:
    ENetHost *server = nullptr;
    ENetHost *client = nullptr;
    ENetPeer *server_peer = nullptr;
    ENetPeer *client_peer = nullptr;
    admission_t admission;
    std::vector<std::string> client_received;
    std::vector<std::string> server_received;

    void
    SetUp() override {
      ASSERT_EQ(enet_initialize(), 0);
      ENetAddress address {};
      ASSERT_EQ(enet_address_set_host(&address, "127.0.0.1"), 0);
      ASSERT_EQ(enet_address_set_port(&address, 0), 0);
      server = enet_host_create(AF_INET, &address, 2, 1, 0, 0);
      client = enet_host_create(AF_INET, &address, 2, 1, 0, 0);
      ASSERT_NE(server, nullptr);
      ASSERT_NE(client, nullptr);
      client_peer = enet_host_connect(client, &server->address, 1, 0);
      ASSERT_NE(client_peer, nullptr);
      for (unsigned i = 0; i < 1000 && !server_peer; ++i) pump();
      ASSERT_NE(server_peer, nullptr);
      for (unsigned i = 0; i < 50; ++i) pump();
      ASSERT_EQ(client_peer->state, ENET_PEER_STATE_CONNECTED);
      ASSERT_EQ(server_peer->state, ENET_PEER_STATE_CONNECTED);
      server->sendAdmissionContext = &admission;
      server->sendAdmission = admission_t::begin;
      server->sendCompletion = admission_t::finish;
    }

    void
    TearDown() override {
      if (server) enet_host_destroy(server);
      if (client) enet_host_destroy(client);
      enet_deinitialize();
    }

    void
    poll(ENetHost *host, bool is_server) {
      ENetEvent event {};
      const auto result = enet_host_service(host, &event, 1);
      ASSERT_GE(result, 0);
      if (result > 0 && event.type == ENET_EVENT_TYPE_CONNECT && is_server) server_peer = event.peer;
      if (result > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) {
        (is_server ? server_received : client_received).emplace_back(reinterpret_cast<const char *>(event.packet->data), event.packet->dataLength);
        enet_packet_destroy(event.packet);
      }
    }
    void
    pump() {
      poll(client, false);
      poll(server, true);
    }
    void
    queue(ENetPeer *peer, const std::string &payload) {
      auto packet = enet_packet_create(payload.data(), payload.size(), ENET_PACKET_FLAG_RELIABLE);
      ASSERT_NE(packet, nullptr);
      ASSERT_EQ(enet_peer_send(peer, 0, packet), 0);
    }
    void
    expire_reliable() {
      ASSERT_FALSE(enet_list_empty(&server_peer->sentReliableCommands));
      auto *command = reinterpret_cast<ENetOutgoingCommand *>(enet_list_front(&server_peer->sentReliableCommands));
      const auto now = enet_time_get();
      command->sentTime = now - command->roundTripTimeout - 1;
      server_peer->nextTimeout = now;
    }
  };

  TEST_F(EnetAdmission, DenialPreservesReliableCommandsAndAcksBeforeSerialization) {
    admission.deny = true;
    queue(server_peer, "server payload");
    queue(client_peer, "client payload");
    enet_host_flush(client);
    for (unsigned i = 0; i < 100 && server_received.empty(); ++i) poll(server, true);
    ASSERT_EQ(server_received.size(), 1U);
    ASSERT_FALSE(enet_list_empty(&server_peer->acknowledgements));
    const auto acks = enet_list_size(&server_peer->acknowledgements);
    const auto commands = enet_list_size(&server_peer->outgoingSendReliableCommands);
    ASSERT_GT(commands, 0U);
    const auto sent_data = server->totalSentData;
    for (unsigned i = 0; i < 10; ++i) enet_host_flush(server);
    EXPECT_EQ(enet_list_size(&server_peer->acknowledgements), acks);
    EXPECT_EQ(enet_list_size(&server_peer->outgoingSendReliableCommands), commands);
    EXPECT_TRUE(enet_list_empty(&server_peer->sentReliableCommands));
    EXPECT_EQ(server->totalSentData, sent_data);
    EXPECT_EQ(admission.admitted, 0U);
    EXPECT_EQ(admission.completed, 0U);
    admission.deny = false;
    for (unsigned i = 0; i < 200 && client_received.empty(); ++i) pump();
    ASSERT_EQ(client_received.size(), 1U);
    EXPECT_EQ(client_received.front(), "server payload");
    EXPECT_TRUE(enet_list_empty(&server_peer->acknowledgements));
    EXPECT_EQ(admission.admitted, admission.completed);
    EXPECT_GT(admission.attempted, 0U);
  }

  TEST_F(EnetAdmission, DenialStillProcessesReliableTimeoutAndDefersRetransmission) {
    queue(server_peer, "timeout payload");
    enet_host_flush(server);
    ASSERT_GT(admission.attempted, 0U);
    admission.deny = true;
    expire_reliable();
    const auto attempted = admission.attempted;
    ASSERT_GE(enet_host_service(server, nullptr, 0), 0);
    EXPECT_TRUE(enet_list_empty(&server_peer->sentReliableCommands));
    EXPECT_FALSE(enet_list_empty(&server_peer->outgoingSendReliableCommands));
    EXPECT_EQ(admission.attempted, attempted);
    admission.deny = false;
    enet_host_flush(server);
    EXPECT_GT(admission.attempted, attempted);
    EXPECT_EQ(admission.admitted, admission.completed);
    for (unsigned i = 0; i < 100 && client_received.empty(); ++i) pump();
    ASSERT_EQ(client_received.size(), 1U);
    EXPECT_EQ(client_received.front(), "timeout payload");
  }

  TEST_F(EnetAdmission, FragmentedReliableDatagramsCompleteOnceAndCountFinalBytes) {
    const std::string payload(8000, 'x');
    queue(server_peer, payload);
    const auto initial_sent = server->totalSentData;
    enet_host_flush(server);
    EXPECT_GT(admission.attempted, 1U);
    EXPECT_EQ(admission.admitted, admission.completed);
    EXPECT_EQ(admission.payload_bytes, server->totalSentData - initial_sent);
    for (unsigned i = 0; i < 300 && client_received.empty(); ++i) pump();
    ASSERT_EQ(client_received.size(), 1U);
    EXPECT_EQ(client_received.front(), payload);
  }

  TEST_F(EnetAdmission, NullCallbacksPreserveLegacyActualReliableDelivery) {
    server->sendAdmission = nullptr;
    server->sendCompletion = nullptr;
    queue(server_peer, "legacy payload");
    for (unsigned i = 0; i < 100 && client_received.empty(); ++i) pump();
    ASSERT_EQ(client_received.size(), 1U);
    EXPECT_EQ(client_received.front(), "legacy payload");
    EXPECT_EQ(admission.admitted, 0U);
  }
}  // namespace
