#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "types.h"

namespace remote_connect::easytier {

  inline constexpr char installation_required_error[] =
    "EasyTier is not installed in its official default location. Install EasyTier separately, then restart Sunshine.";

  // The folder Sunshine keeps its state in, where the files of the remote
  // connection modules live. The rest of these modules ask here instead of
  // including the platform header themselves: that header brings in
  // Boost.Asio and Boost.Process, which they otherwise do not need.
  std::filesystem::path
  state_directory();

  bool
  virtual_subnet_conflicts(const std::string &virtual_ip);

  class runtime_t {
  public:
    runtime_t();
    ~runtime_t();

    runtime_t(const runtime_t &) = delete;
    runtime_t &
    operator=(const runtime_t &) = delete;

    bool
    available() const;

    bool
    running(std::string &error);

    // Starts the core with `authorized_client_keys` in force: the EasyTier
    // public keys of the devices this host authorizes, as lowercase hex. An
    // empty set authorizes no device. The set is passed in on every start, so
    // it always reflects what the caller currently considers authorized.
    bool
    start(
      const enrollment_t &enrollment,
      const std::vector<std::string> &authorized_client_keys,
      const std::string &hostname,
      std::string &error
    );

    void
    stop();

  private:
    struct state_t;
    std::unique_ptr<state_t> state_;
  };

}  // namespace remote_connect::easytier
