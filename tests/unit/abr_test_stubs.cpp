#include <boost/log/sources/severity_logger.hpp>

#include "src/ai/proxy.h"
#include "src/config.h"

#ifndef _WIN32
  #include "src/platform/linux/foreground_app.h"
#endif

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

#ifndef _WIN32
// abr.cpp's Linux foreground lookup goes through the compositor-specific
// backend (KWin D-Bus script / niri CLI), which needs a live session and is not
// part of this compile/link target. Stub the cached report so the target links;
// the Linux behaviour itself is covered by the foreground_app test target.
namespace platf::foreground_app {
  info_t
  detect() {
    return {};
  }
}  // namespace platf::foreground_app
#endif
