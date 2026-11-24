#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace boost::asio::ip {
  class address;
}

namespace platf {
  struct buffer_descriptor_t {
    const char *buffer;
    size_t size;

    // Constructors required for emplace_back() prior to C++20
    buffer_descriptor_t(const char *buffer, size_t size):
        buffer(buffer),
        size(size) {
    }

    buffer_descriptor_t():
        buffer(nullptr),
        size(0) {
    }
  };

  struct batched_send_info_t {
    // Optional headers to be prepended to each packet
    const char *headers;
    size_t header_size;

    // One or more data buffers to use for the payloads
    //
    // NB: Data buffers must be aligned to payload size!
    std::vector<buffer_descriptor_t> &payload_buffers;
    size_t payload_size;

    // The offset (in header+payload message blocks) in the header and payload
    // buffers to begin sending messages from
    size_t block_offset;

    // The number of header+payload message blocks to send
    size_t block_count;

    std::uintptr_t native_socket;
    boost::asio::ip::address &target_address;
    uint16_t target_port;
    const boost::asio::ip::address &source_address;
    bool would_block = false;  ///< Output for a send rejected by local socket backpressure.

    /**
     * @brief Returns a payload buffer descriptor for the given payload offset.
     * @param offset The offset in the total payload data (bytes).
     * @return Buffer descriptor describing the region at the given offset.
     */
    buffer_descriptor_t
    buffer_for_payload_offset(ptrdiff_t offset) {
      for (const auto &desc : payload_buffers) {
        if (offset < desc.size) {
          return {
            desc.buffer + offset,
            desc.size - offset,
          };
        }
        else {
          offset -= desc.size;
        }
      }
      return {};
    }
  };

  bool
  send_batch(batched_send_info_t &send_info);

  struct send_info_t {
    const char *header;
    size_t header_size;
    const char *payload;
    size_t payload_size;

    std::uintptr_t native_socket;
    boost::asio::ip::address &target_address;
    uint16_t target_port;
    const boost::asio::ip::address &source_address;
    bool would_block = false;  ///< Output for a send rejected by local socket backpressure.
  };

  bool
  send(send_info_t &send_info);
}  // namespace platf
