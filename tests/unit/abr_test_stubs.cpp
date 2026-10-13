#include <boost/log/sources/severity_logger.hpp>

#include "src/ai/proxy.h"
#include "src/config.h"

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
  input_t input;
  sunshine_t sunshine;
}  // namespace config

namespace ai {

  bool
  isAiEnabled() {
    return false;
  }

  AiProxyResult
  processAiChat(const std::string &) {
    return { 200, "{}", "application/json" };
  }

  AiProxyResult
  processAiChatStream(const std::string &, std::function<bool(const char *, size_t)>, std::function<bool()>) {
    return { 200, "", "text/event-stream" };
  }

}  // namespace ai
