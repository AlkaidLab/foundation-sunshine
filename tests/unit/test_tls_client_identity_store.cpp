/**
 * @file tests/unit/test_tls_client_identity_store.cpp
 * @brief Tests for connection-scoped nvhttp client identities.
 */

#include <gtest/gtest.h>

#include "src/nvhttp/tls_client_identity_store.h"

TEST(TlsClientIdentityStore, IdentitySurvivesMultipleRequestsOnOneConnection) {
  nvhttp::tls_client_identity_store_t store;
  auto connection = std::make_shared<int>(1);

  store.remember("connection-a", connection, "client-a");

  EXPECT_EQ(store.lookup("connection-a"), "client-a");
  EXPECT_EQ(store.lookup("connection-a"), "client-a");
}

TEST(TlsClientIdentityStore, RejectsInvalidRememberInputs) {
  nvhttp::tls_client_identity_store_t store;
  auto connection = std::make_shared<int>(1);
  std::weak_ptr<void> expired_connection;
  {
    auto lifetime = std::make_shared<int>(2);
    expired_connection = lifetime;
  }

  ASSERT_TRUE(expired_connection.expired());
  store.remember("", connection, "client-without-key");
  store.remember("expired-connection", expired_connection, "client-expired");
  store.remember("empty-uuid", connection, "");

  EXPECT_TRUE(store.lookup("").empty());
  EXPECT_TRUE(store.lookup("expired-connection").empty());
  EXPECT_TRUE(store.lookup("empty-uuid").empty());
}

TEST(TlsClientIdentityStore, DropsIdentityAfterConnectionExpires) {
  nvhttp::tls_client_identity_store_t store;
  auto connection = std::make_shared<int>(1);
  store.remember("connection-a", connection, "client-a");

  connection.reset();

  EXPECT_TRUE(store.lookup("connection-a").empty());
}

TEST(TlsClientIdentityStore, ReusedConnectionKeyKeepsNewestIdentity) {
  nvhttp::tls_client_identity_store_t store;
  auto old_connection = std::make_shared<int>(1);
  auto new_connection = std::make_shared<int>(2);
  store.remember("connection-a", old_connection, "client-a");
  store.remember("connection-a", new_connection, "client-b");

  old_connection.reset();

  EXPECT_EQ(store.lookup("connection-a"), "client-b");
}

TEST(TlsClientIdentityStore, ForgetRemovesOnlyTheSelectedConnection) {
  nvhttp::tls_client_identity_store_t store;
  auto connection_a = std::make_shared<int>(1);
  auto connection_b = std::make_shared<int>(2);
  store.remember("connection-a", connection_a, "client-a");
  store.remember("connection-b", connection_b, "client-b");

  store.forget("connection-a");

  EXPECT_TRUE(store.lookup("connection-a").empty());
  EXPECT_EQ(store.lookup("connection-b"), "client-b");
}
