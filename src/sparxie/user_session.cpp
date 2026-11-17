#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>

  #include <boost/filesystem/path.hpp>
  #include <boost/process/v1.hpp>
#endif

#include "src/sparxie/user_session.h"

#if defined(_WIN32)
  #include "src/logging.h"
  #include "src/platform/run_command.h"
  #include "src/platform/windows/misc.h"

  #include <exception>
#endif

namespace sparxie {
#if defined(_WIN32)
  UserRunner
  make_user_runner() {
    return [](const UserCall &call) {
      // Impersonation does not nest: reverting inside would drop the outer one.
      thread_local int depth = 0;
      if (depth > 0 || !platf::is_running_as_system()) {
        call();
        return true;
      }
      HANDLE token = platf::retrieve_users_token(false);
      if (!token) return false;
      std::exception_ptr failure;
      ++depth;
      const std::error_code ec = platf::impersonate_current_user(token, [&] {
        try {
          call();
        }
        catch (...) {
          failure = std::current_exception();
        }
      });
      --depth;
      CloseHandle(token);
      if (ec) return false;
      if (failure) std::rethrow_exception(failure);
      return true;
    };
  }

  Status
  launch_in_user_session(const fs::path &exe) {
    boost::filesystem::path working_dir = exe.parent_path().wstring();
    const std::string command = "\"" + exe.string() + "\"";
    std::error_code ec;
    auto environment = boost::this_process::environment();
    // Elevated like the host's own elevated applications, with no UAC prompt (the host holds the rights). The file picker of Sparxie.Desktop is the Win32 IFileOpenDialog, which works elevated; the WinRT picker did not.
    auto child = platf::run_command(true, false, command, working_dir, environment, nullptr, ec, nullptr);
    if (ec) {
      BOOST_LOG(warning) << "Sparxie did not start in the user session: " << ec.message();
      return Status::fail(500, "launch_failed", ec.message());
    }
    child.detach();
    return Status::success();
  }
#else
  UserRunner
  make_user_runner() { return {}; }

  Status
  launch_in_user_session(const fs::path &) { return Status::fail(409, "launch_unavailable", "Sparxie only runs on Windows"); }
#endif
}  // namespace sparxie
