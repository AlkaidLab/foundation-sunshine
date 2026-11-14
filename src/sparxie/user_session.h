#pragma once
#include "src/sparxie/platform.h"

namespace sparxie {
  // The host is a service on Windows (SYSTEM). These two use the host's
  // existing launch-as-user path (platf::retrieve_users_token,
  // platf::impersonate_current_user, platf::run_command), so the Sparxie pages
  // see the registry and folders of the signed-in person, and Sparxie, a
  // window, starts in that person's session and never as the service.
  //
  // The runner is empty when the host is not Windows.
  UserRunner
  make_user_runner();

  // Starts a program in the signed-in user's session without waiting for it.
  Status
  launch_in_user_session(const fs::path &exe);
}  // namespace sparxie
