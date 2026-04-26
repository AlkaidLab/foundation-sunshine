#include <boost/log/sources/severity_logger.hpp>

#include "src/config.h"
#include "src/confighttp.h"

boost::log::sources::severity_logger<int> verbose;
boost::log::sources::severity_logger<int> debug;
boost::log::sources::severity_logger<int> info;
boost::log::sources::severity_logger<int> warning;
boost::log::sources::severity_logger<int> error;
boost::log::sources::severity_logger<int> fatal;
#ifdef SUNSHINE_TESTS
boost::log::sources::severity_logger<int> tests;
#endif

namespace config {
  video_t video;
  audio_t audio;
  stream_t stream;
  nvhttp_t nvhttp;
  webhook_t webhook;
  input_t input;
  sunshine_t sunshine;
}  // namespace config

namespace confighttp {

  bool
  isAiEnabled() {
    return false;
  }

  AiProxyResult
  processAiChat(const std::string &) {
    return {200, "{}", "application/json"};
  }

  AiProxyResult
  processAiChatStream(const std::string &, std::function<void(const char *, size_t)>) {
    return {200, "", "text/event-stream"};
  }

}  // namespace confighttp