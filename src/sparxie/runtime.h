#pragma once
#include "src/sparxie/host.h"
#include <string>
namespace sparxie {
  HostEnv
  make_host_env();
  Rgba
  decode_art_image(std::string_view bytes);
  // Requests return immediately; callers share one in-flight refresh.
  json
  request_art_refresh(bool force = false);
  json
  art_refresh_status(std::uint64_t job_id);
  void
  stop_art_refresh();
  void
  note_host_started();
  // Runs before an application's command starts: writes the game's settings and display for this stream (`session` is the mode the client asked
  // for) and returns the arguments to append to the command (empty for most apps).
  // Runs when the application ends (the game quit, or the stream ended and the host ended it): puts back the game settings its launch wrote. Returns at once; a thread of its
  // own waits for the game process to be gone first.
  void
  after_session(const std::string &app_name, const std::string &cmd);
  std::string
  before_launch(const std::string &app_name, const std::string &cmd, const SessionMode &session = {});
}  // namespace sparxie
