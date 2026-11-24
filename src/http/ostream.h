/**
 * @file src/http/ostream.h
 * @brief Synchronous Beast serialization into an application response buffer.
 */
#pragma once
#include <boost/asio/buffer.hpp>
#include <boost/system/system_error.hpp>
#include <ostream>

namespace web {
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
}  // namespace web
