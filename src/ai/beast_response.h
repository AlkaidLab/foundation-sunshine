/**
 * @file src/ai/beast_response.h
 * @brief Beast HTTP framing into the existing HTTPS response buffer.
 */
#pragma once

#include <boost/asio/write.hpp>
#include <boost/beast/http.hpp>
#include <ostream>
#include <string>

namespace ai::http::framing {
  // This SyncWriteStream only fills an ostream buffer. The owning transport
  // still flushes asynchronously and waits for its send completion.
  class BufferedWriteStream {
    std::ostream &output;

  public:
    explicit BufferedWriteStream(std::ostream &output): output(output) {}

    template <class ConstBufferSequence>
    size_t
    write_some(const ConstBufferSequence &buffers, boost::system::error_code &ec) {
      ec.clear();
      size_t written = 0;
      for (auto it = boost::asio::buffer_sequence_begin(buffers); it != boost::asio::buffer_sequence_end(buffers); ++it) {
        const boost::asio::const_buffer buffer = *it;
        output.write(static_cast<const char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        if (!output) {
          ec = make_error_code(boost::system::errc::io_error);
          return written;
        }
        written += buffer.size();
      }
      return written;
    }

    template <class ConstBufferSequence>
    size_t
    write_some(const ConstBufferSequence &buffers) {
      boost::system::error_code ec;
      const auto written = write_some(buffers, ec);
      if (ec) throw boost::system::system_error(ec);
      return written;
    }
  };

  inline void
  sse_header(std::ostream &output) {
    namespace http = boost::beast::http;
    http::response<http::empty_body> message { http::status::ok, 11 };
    message.keep_alive(false);
    message.chunked(true);
    message.set(http::field::content_type, "text/event-stream");
    message.set(http::field::cache_control, "no-cache");
    http::response_serializer<http::empty_body> serializer { message };
    BufferedWriteStream stream { output };
    http::write_header(stream, serializer);
  }

  inline void
  sse_chunk(std::ostream &output, const char *data, size_t length) {
    // An empty data callback must not become the HTTP terminating chunk.
    if (!length) return;
    BufferedWriteStream stream { output };
    boost::asio::write(stream, boost::beast::http::make_chunk(boost::asio::const_buffer(data, length)));
  }

  inline void
  sse_end(std::ostream &output) {
    BufferedWriteStream stream { output };
    boost::asio::write(stream, boost::beast::http::make_chunk_last());
  }

  inline void
  reply(std::ostream &output, int status, std::string body, const std::string &type, bool at_capacity = false) {
    namespace http = boost::beast::http;
    http::response<http::string_body> message { static_cast<http::status>(status), 11 };
    message.keep_alive(false);
    message.set(http::field::content_type, type);
    if (at_capacity) message.set(http::field::retry_after, "1");
    message.body() = std::move(body);
    message.prepare_payload();
    BufferedWriteStream stream { output };
    http::write(stream, message);
  }
}  // namespace ai::http::framing
