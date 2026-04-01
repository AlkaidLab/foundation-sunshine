/**
 * @file src/subprocess/ipc_pipe.h
 * @brief Named pipe IPC implementation for subprocess communication.
 *
 * This file provides cross-platform named pipe abstraction for communication
 * between the main process (server) and subprocess (client).
 */
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ipc_protocol.h"

namespace subprocess {
  namespace ipc {

    /**
     * @brief Result codes for IPC operations.
     */
    enum class result_e {
      success,
      error_create_pipe,
      error_connect,
      error_timeout,
      error_disconnected,
      error_invalid_message,
      error_write,
      error_read,
    };

    /**
     * @brief Convert result code to string.
     */
    const char *
    result_to_string(result_e result);

    /**
     * @brief Callback type for received messages.
     * @param header The message header.
     * @param payload The message payload data.
     * @return true to continue receiving, false to stop.
     */
    using message_callback_t = std::function<bool(const message_header_t &header, const std::vector<uint8_t> &payload)>;

    /**
     * @brief IPC Pipe Server - runs in main process.
     *
     * Creates a named pipe and waits for subprocess to connect.
     */
    class pipe_server_t {
    public:
      pipe_server_t();
      ~pipe_server_t();

      // Non-copyable
      pipe_server_t(const pipe_server_t &) = delete;
      pipe_server_t &operator=(const pipe_server_t &) = delete;

      /**
       * @brief Create the named pipe server.
       * @param session_id Session ID for pipe name.
       * @return Result code.
       */
      result_e
      create(uint32_t session_id);

      /**
       * @brief Wait for client connection.
       * @param timeout_ms Timeout in milliseconds.
       * @return Result code.
       */
      result_e
      wait_for_connection(int timeout_ms);

      /**
       * @brief Send a message to the client.
       * @param type Message type.
       * @param payload Payload data (can be nullptr if payload_length is 0).
       * @param payload_length Length of payload.
       * @return Result code.
       */
      result_e
      send_message(message_type_e type, const void *payload = nullptr, size_t payload_length = 0);

      /**
       * @brief Receive a message from the client.
       * @param header Output message header.
       * @param payload Output payload buffer.
       * @param timeout_ms Timeout in milliseconds.
       * @return Result code.
       */
      result_e
      receive_message(message_header_t &header, std::vector<uint8_t> &payload, int timeout_ms);

      /**
       * @brief Start async receive loop.
       * @param callback Callback for received messages.
       */
      void
      start_receive_loop(message_callback_t callback);

      /**
       * @brief Stop async receive loop.
       */
      void
      stop_receive_loop();

      /**
       * @brief Check if connected to client.
       */
      bool
      is_connected() const;

      /**
       * @brief Close the pipe.
       */
      void
      close();

      /**
       * @brief Get the pipe name.
       */
      const std::string &
      pipe_name() const {
        return pipe_name_;
      }

    private:
      std::string pipe_name_;
      void *pipe_handle_ = nullptr;  // Platform-specific handle
      std::atomic<bool> connected_ { false };
      std::atomic<bool> running_ { false };
      std::thread receive_thread_;
      std::mutex write_mutex_;
      uint32_t sequence_number_ = 0;
    };

    /**
     * @brief IPC Pipe Client - runs in subprocess.
     *
     * Connects to the named pipe created by main process.
     */
    class pipe_client_t {
    public:
      pipe_client_t();
      ~pipe_client_t();

      // Non-copyable
      pipe_client_t(const pipe_client_t &) = delete;
      pipe_client_t &operator=(const pipe_client_t &) = delete;

      /**
       * @brief Connect to the named pipe server.
       * @param session_id Session ID for pipe name.
       * @param timeout_ms Connection timeout in milliseconds.
       * @return Result code.
       */
      result_e
      connect(uint32_t session_id, int timeout_ms);

      /**
       * @brief Send a message to the server.
       * @param type Message type.
       * @param payload Payload data (can be nullptr if payload_length is 0).
       * @param payload_length Length of payload.
       * @return Result code.
       */
      result_e
      send_message(message_type_e type, const void *payload = nullptr, size_t payload_length = 0);

      /**
       * @brief Receive a message from the server.
       * @param header Output message header.
       * @param payload Output payload buffer.
       * @param timeout_ms Timeout in milliseconds.
       * @return Result code.
       */
      result_e
      receive_message(message_header_t &header, std::vector<uint8_t> &payload, int timeout_ms);

      /**
       * @brief Start async receive loop.
       * @param callback Callback for received messages.
       */
      void
      start_receive_loop(message_callback_t callback);

      /**
       * @brief Stop async receive loop.
       */
      void
      stop_receive_loop();

      /**
       * @brief Check if connected to server.
       */
      bool
      is_connected() const;

      /**
       * @brief Disconnect from server.
       */
      void
      disconnect();

    private:
      std::string pipe_name_;
      void *pipe_handle_ = nullptr;  // Platform-specific handle
      std::atomic<bool> connected_ { false };
      std::atomic<bool> running_ { false };
      std::thread receive_thread_;
      std::mutex write_mutex_;
      uint32_t sequence_number_ = 0;
    };

  }  // namespace ipc
}  // namespace subprocess
