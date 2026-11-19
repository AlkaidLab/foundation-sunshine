/**
 * @file src/platform/windows/touch_keyboard_session.cpp
 * @brief Publish session-scoped touch keyboard preferences to the GUI.
 */
#include "src/touch_keyboard_session.h"

#include "src/logging.h"
#include "src/tray/tray_state.h"

#include <Windows.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

namespace touch_kb {
  namespace {
    class manager_t;

    struct lease_t {
      explicit lease_t(std::shared_ptr<manager_t> manager): manager(std::move(manager)) {}
      ~lease_t();
      std::shared_ptr<manager_t> manager;
      std::uint64_t id { 0 };
    };

    class manager_t: public std::enable_shared_from_this<manager_t> {
    public:
      std::shared_ptr<void> acquire(bool enabled) {
        auto lease = std::make_shared<lease_t>(shared_from_this());
        std::lock_guard guard { mutex };
        if (requests.empty() && !ProcessIdToSessionId(GetCurrentProcessId(), &capture_session)) {
          BOOST_LOG(warning) << "touch_kb: cannot identify the capture session";
          return {};
        }
        const auto id = ++next_id;
        requests.emplace(id, enabled);
        lease->id = id;
        if (requests.begin()->first == id) {
          publish();
        }
        else if (requests.begin()->second != enabled) {
          BOOST_LOG(info) << "touch_kb: conflicting client preference waits for the earlier session";
        }
        return lease;
      }

      void release(std::uint64_t id) noexcept {
        try {
          std::lock_guard guard { mutex };
          const auto it = requests.find(id);
          if (it == requests.end()) return;
          const bool was_first = it == requests.begin();
          const bool previous = requests.begin()->second;
          requests.erase(it);
          if (requests.empty() || (was_first && requests.begin()->second != previous)) publish();
        }
        catch (...) {
          BOOST_LOG(warning) << "touch_kb: could not publish the session preference";
        }
      }

    private:
      void publish() {
        if (requests.empty()) {
          tray_state::set_touch_keyboard(std::nullopt);
        }
        else {
          tray_state::set_touch_keyboard(tray_state::touch_keyboard_t { requests.begin()->second, capture_session });
        }
      }

      std::mutex mutex;
      std::map<std::uint64_t, bool> requests;
      std::uint64_t next_id { 0 };
      DWORD capture_session { 0 };
    };

    lease_t::~lease_t() {
      if (id) manager->release(id);
    }

    std::shared_ptr<manager_t>
    manager() {
      static auto instance = std::make_shared<manager_t>();
      return instance;
    }
  }  // namespace

  std::shared_ptr<void>
  acquire(int intent) noexcept {
    if (intent != 0 && intent != 1) return {};
    try {
      return manager()->acquire(intent == 1);
    }
    catch (...) {
      BOOST_LOG(warning) << "touch_kb: unable to publish the client preference";
      return {};
    }
  }
}  // namespace touch_kb
