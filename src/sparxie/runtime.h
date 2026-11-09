#pragma once
#include "src/sparxie/host.h"
#include <string>
namespace sparxie {
  HostEnv
  make_host_env();
  void
  note_host_started();
  void
  note_app_list_opened();
  // Runs before an application's command starts: writes the game's settings and display for this stream (`session` is the mode the client asked
  // for) and returns the arguments to append to the command (empty for most apps).
  // Runs when the application ends (the game quit, or the stream ended and the host ended it): puts back the game settings its launch wrote. Returns at once; a thread of its
  // own waits for the game process to be gone first.
  void
  after_session(const std::string &app_name, const std::string &cmd);
  std::string
  before_launch(const std::string &app_name, const std::string &cmd, const SessionMode &session = {});
}  // namespace sparxie
