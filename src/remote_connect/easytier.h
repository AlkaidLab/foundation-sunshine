#pragma once

#include <memory>
#include <string>

#include "types.h"

namespace remote_connect::easytier {

  inline constexpr char installation_required_error[] =
    "EasyTier is not installed in its official default location. Install EasyTier separately, then restart Sunshine.";

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

    bool
    start(const enrollment_t &enrollment, const std::string &hostname, std::string &error);

    void
    stop();

  private:
    struct state_t;
    std::unique_ptr<state_t> state_;
  };

}  // namespace remote_connect::easytier
