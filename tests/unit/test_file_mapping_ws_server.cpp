/**
 * @file tests/unit/test_file_mapping_ws_server.cpp
 * @brief Test src/file_mapping_ws_server.*.
 */
#include <src/file_mapping/file_mapping_ws_server.h>

#include <gtest/gtest.h>

TEST(FileMappingWsServer, StartRequiresSharedOwnership) {
  boost::asio::io_context io;
  file_mapping_ws::transport_config_t config;
  config.certificate_file = "cert.pem";
  config.private_key_file = "key.pem";

  file_mapping_ws::server_t server(io, config);
  auto result = server.start();
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("std::shared_ptr"), std::string::npos);
  EXPECT_EQ(server.state(), file_mapping_ws::transport_state_e::stopped);
}
