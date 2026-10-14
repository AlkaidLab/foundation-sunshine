/**
 * @file src/ai/beast_response.h
 * @brief Beast HTTP framing into the existing HTTPS response buffer.
 */
#pragma once

#include "src/http/ostream.h"
#include <boost/asio/write.hpp>
#include <boost/beast/http.hpp>
#include <ostream>
#include <string>

namespace ai::http::framing {
  using BufferedWriteStream = web::BufferedWriteStream;

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
