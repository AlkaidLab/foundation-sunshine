#include "src/platform/linux/xrandr_display.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#include <boost/log/trivial.hpp>

#include "src/platform/common.h"

namespace platf::xrandr {

  namespace {
    /// Probe results are cached: a session's display server does not change, and
    /// re-opening the display on every query would be both slow and noisy.
    std::mutex probe_mutex;
    std::optional<bool> cached_support;
    std::chrono::steady_clock::time_point cooldown_until;

    constexpr auto kRetryCooldown = std::chrono::milliseconds { 20'000 };

    /// 96 dpi, the screen-size millimetres RandR expects for a given pixel size.
    constexpr double kMillimetresPerPixel = 25.4 / 96.0;

    int
    millimetres_for(int pixels) {
      return static_cast<int>(std::lround(pixels * kMillimetresPerPixel));
    }

    /// Xlib reports errors asynchronously, through a process-wide handler.
    /// Only the apply path writes, so a thread-local flag under a lock is
    /// enough, and the previous handler is always restored.
    std::mutex xlib_mutex;
    thread_local int xlib_error_code = 0;

    int
    record_xlib_error(Display *, XErrorEvent *event) {
      if (!xlib_error_code) {
        xlib_error_code = event->error_code;
      }
      return 0;
    }

    struct display_handle_t {
      Display *display = nullptr;

      display_handle_t() = default;
      display_handle_t(const display_handle_t &) = delete;
      display_handle_t &operator=(const display_handle_t &) = delete;

      ~display_handle_t() {
        if (display) {
          XCloseDisplay(display);
        }
      }

      bool
      open() {
        display = XOpenDisplay(nullptr);
        return display != nullptr;
      }
    };

    /// RandR version and extension check; fills nothing else.
    bool
    randr_usable(Display *display) {
      int event_base = 0;
      int error_base = 0;
      if (!XRRQueryExtension(display, &event_base, &error_base)) {
        return false;
      }
      int major = 0;
      int minor = 0;
      if (!XRRQueryVersion(display, &major, &minor)) {
        return false;
      }
      return major > 1 || (major == 1 && minor >= 2);
    }

    std::string
    format_refresh(double refresh) {
      std::ostringstream formatted;
      formatted << std::fixed << std::setprecision(2) << refresh;
      return formatted.str();
    }

    std::string
    format_mode_id(int width, int height, double refresh) {
      return std::to_string(width) + "x" + std::to_string(height) + "@" + format_refresh(refresh);
    }

    int
    rotation_turns(Rotation rotation) {
      if (rotation & RR_Rotate_90) {
        return 1;
      }
      if (rotation & RR_Rotate_180) {
        return 2;
      }
      if (rotation & RR_Rotate_270) {
        return 3;
      }
      return 0;
    }

    Rotation
    rotation_from_turns(int turns, bool reflect_x, bool reflect_y) {
      Rotation rotation = RR_Rotate_0;
      switch (((turns % 4) + 4) % 4) {
        case 1:
          rotation = RR_Rotate_90;
          break;
        case 2:
          rotation = RR_Rotate_180;
          break;
        case 3:
          rotation = RR_Rotate_270;
          break;
        default:
          break;
      }
      if (reflect_x) {
        rotation |= RR_Reflect_X;
      }
      if (reflect_y) {
        rotation |= RR_Reflect_Y;
      }
      return rotation;
    }

    /// What a crtc currently scans, kept for the rollback path.
    struct crtc_snapshot_t {
      RRCrtc crtc = None;
      RRMode mode = None;
      Rotation rotation = RR_Rotate_0;
      int x = 0;
      int y = 0;
      std::vector<RROutput> outputs;
      bool enabled = false;
    };

    struct screen_snapshot_t {
      int width = 0;
      int height = 0;
      int width_mm = 0;
      int height_mm = 0;
    };

    std::chrono::steady_clock::time_point
    now() {
      return std::chrono::steady_clock::now();
    }
  }  // namespace

  double
  refresh_from_timings(std::uint64_t dot_clock, int htotal, int vtotal) {
    // RandR stores the dot clock in Hz, so the refresh is the pixel clock divided
    // by the total number of pixels per frame. Interlaced modes are reported as
    // the same formula: their interlaced flag changes nothing about the rate the
    // server will scan them at.
    if (htotal <= 0 || vtotal <= 0 || dot_clock == 0) {
      return 0.0;
    }
    return static_cast<double>(dot_clock) / (static_cast<double>(htotal) * static_cast<double>(vtotal));
  }

  bool
  probe_allowed() {
    std::lock_guard lock(probe_mutex);
    return now() >= cooldown_until;
  }

  void
  note_available() {
    std::lock_guard lock(probe_mutex);
    cached_support = true;
    cooldown_until = {};
  }

  void
  note_unavailable(const std::string &reason) {
    std::lock_guard lock(probe_mutex);
    cached_support = false;
    cooldown_until = now() + kRetryCooldown;
    BOOST_LOG(debug) << "xrandr output management unavailable (" << reason << ")";
  }

  bool
  session_supports_xrandr() {
    {
      std::lock_guard lock(probe_mutex);
      if (cached_support) {
        return *cached_support;
      }
      if (now() < cooldown_until) {
        return false;
      }
    }

    display_handle_t handle;
    if (!handle.open() || !randr_usable(handle.display)) {
      note_unavailable("no X display with RandR 1.2");
      return false;
    }

    note_available();
    return true;
  }

  std::optional<state_t>
  query_state() {
    display_handle_t handle;
    if (!handle.open() || !randr_usable(handle.display)) {
      note_unavailable("no X display with RandR 1.2");
      return std::nullopt;
    }

    const Window root = DefaultRootWindow(handle.display);
    XRRScreenResources *resources = XRRGetScreenResourcesCurrent(handle.display, root);
    if (!resources) {
      note_unavailable("XRRGetScreenResourcesCurrent failed");
      return std::nullopt;
    }

    state_t state;
    state.screen_width = DisplayWidth(handle.display, DefaultScreen(handle.display));
    state.screen_height = DisplayHeight(handle.display, DefaultScreen(handle.display));

    const RROutput primary = XRRGetOutputPrimary(handle.display, root);

    for (int i = 0; i < resources->noutput; ++i) {
      const RROutput output_id = resources->outputs[i];
      XRROutputInfo *info = XRRGetOutputInfo(handle.display, resources, output_id);
      if (!info) {
        continue;
      }

      output_t output;
      output.name = info->name ? info->name : "";
      output.connected = info->connection == RR_Connected;
      output.primary = output_id == primary;

      for (int mode_index = 0; mode_index < info->nmode; ++mode_index) {
        // The output's mode list holds indices into the resources' mode array,
        // and a server that just performed a modeset can briefly hand back a
        // list that no longer matches. Trusting it blindly crashes the host.
        const auto resources_mode = info->modes[mode_index];
        if (resources_mode >= resources->nmode) {
          BOOST_LOG(debug) << "XRandR reported mode index " << resources_mode << " out of "
                           << resources->nmode << " for output " << (info->name ? info->name : "");
          continue;
        }
        const auto &mode_info = resources->modes[resources_mode];
        mode_t mode;
        mode.width = static_cast<int>(mode_info.width);
        mode.height = static_cast<int>(mode_info.height);
        mode.refresh = refresh_from_timings(mode_info.dotClock, mode_info.hTotal, mode_info.vTotal);
        mode.id = format_mode_id(mode.width, mode.height, mode.refresh);
        output.modes.push_back(std::move(mode));
      }

      if (info->crtc != None) {
        XRRCrtcInfo *crtc = XRRGetCrtcInfo(handle.display, resources, info->crtc);
        if (crtc) {
          output.enabled = crtc->mode != None;
          output.x = static_cast<int>(crtc->x);
          output.y = static_cast<int>(crtc->y);
          output.width = static_cast<int>(crtc->width);
          output.height = static_cast<int>(crtc->height);
          output.rotation = rotation_turns(crtc->rotation);
          output.reflect_x = (crtc->rotation & RR_Reflect_X) != 0;
          output.reflect_y = (crtc->rotation & RR_Reflect_Y) != 0;

          if (output.enabled) {
            // `crtc->mode` is an RRMode handle, not an index into the mode
            // array: the timing it names has to be looked up by id.
            for (int mi = 0; mi < resources->nmode; ++mi) {
              const auto &mode_info = resources->modes[mi];
              if (mode_info.id != crtc->mode) {
                continue;
              }
              output.refresh = refresh_from_timings(mode_info.dotClock, mode_info.hTotal, mode_info.vTotal);
              output.mode_id = format_mode_id(static_cast<int>(mode_info.width),
                                               static_cast<int>(mode_info.height),
                                               output.refresh);
              break;
            }

            // A timing the server scans but the output does not advertise would
            // leave the entry without a current mode; record it so the state is
            // self-consistent instead of half-known.
            const bool known = std::any_of(output.modes.begin(), output.modes.end(),
                                           [&](const mode_t &mode) { return mode.id == output.mode_id; });
            if (!known && !output.mode_id.empty()) {
              mode_t current;
              current.id = output.mode_id;
              current.width = output.width;
              current.height = output.height;
              current.refresh = output.refresh;
              current.current = true;
              output.modes.push_back(std::move(current));
            }
          }

          // Mirroring is a shared crtc. The first output on it is the group's
          // leader and points at nobody; every later member points at the
          // leader, so a group can be rebuilt from any of its members.
          if (crtc->noutput > 1 && crtc->outputs[0] != output_id) {
            XRROutputInfo *leader_info = XRRGetOutputInfo(handle.display, resources, crtc->outputs[0]);
            if (leader_info) {
              output.mirror_of = leader_info->name ? leader_info->name : "";
              XRRFreeOutputInfo(leader_info);
            }
          }

          XRRFreeCrtcInfo(crtc);
        }
      }

      for (auto &mode : output.modes) {
        mode.current = !output.mode_id.empty() && mode.id == output.mode_id;
      }

      XRRFreeOutputInfo(info);
      state.outputs.push_back(std::move(output));
    }

    XRRFreeScreenResources(resources);
    note_available();
    return state;
  }

  const output_t *
  find_output(const state_t &state, const std::string &name) {
    const auto it = std::find_if(state.outputs.begin(), state.outputs.end(),
                                 [&](const output_t &output) { return output.name == name; });
    return it == state.outputs.end() ? nullptr : &*it;
  }

  output_t *
  find_output(state_t &state, const std::string &name) {
    const auto it = std::find_if(state.outputs.begin(), state.outputs.end(),
                                 [&](const output_t &output) { return output.name == name; });
    return it == state.outputs.end() ? nullptr : &*it;
  }

  const mode_t *
  current_mode(const output_t &output) {
    if (output.mode_id.empty()) {
      return nullptr;
    }
    const auto it = std::find_if(output.modes.begin(), output.modes.end(),
                                 [&](const mode_t &mode) { return mode.id == output.mode_id; });
    return it == output.modes.end() ? nullptr : &*it;
  }

  const mode_t *
  matching_mode(const output_t &output, int width, int height, double refresh) {
    const mode_t *closest = nullptr;
    double closest_delta = 0.0;
    for (const auto &mode : output.modes) {
      if (mode.width != width || mode.height != height) {
        continue;
      }
      const double delta = std::abs(mode.refresh - refresh);
      // A refresh of zero means "whatever this size runs at": take the first
      // match, which is the order the server listed them in.
      if (refresh <= 0.0) {
        return &mode;
      }
      if (!closest || delta < closest_delta) {
        closest = &mode;
        closest_delta = delta;
      }
    }
    // RandR reports refresh as timings-derived and rounded differently between
    // drivers, so an exact match is not something to insist on.
    return closest && closest_delta <= 1.0 ? closest : nullptr;
  }

  bool
  plan_for(const state_t &desired, plan_t &out, std::string &error) {
    out = {};

    // A deterministic order keeps the plan stable across calls, and it decides
    // which member of a mirror group provides the mode and position.
    std::vector<const output_t *> ordered;
    ordered.reserve(desired.outputs.size());
    for (const auto &output : desired.outputs) {
      ordered.push_back(&output);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const output_t *lhs, const output_t *rhs) { return lhs->name < rhs->name; });

    std::vector<crtc_request_t> requests;
    std::string primary;

    for (const auto *output : ordered) {
      if (!output->enabled) {
        continue;
      }

      // Where does this output belong? Either its own group, or the group of the
      // output it mirrors.
      std::string leader_name = output->name;
      if (!output->mirror_of.empty()) {
        const auto *leader = find_output(desired, output->mirror_of);
        if (!leader) {
          error = "output " + output->name + " mirrors unknown output " + output->mirror_of;
          return false;
        }
        if (!leader->enabled) {
          error = "output " + output->name + " mirrors disabled output " + output->mirror_of;
          return false;
        }
        leader_name = leader->name;
      }

      auto request = std::find_if(requests.begin(), requests.end(),
                                  [&](const crtc_request_t &candidate) {
                                    return candidate.outputs.front() == leader_name;
                                  });

      if (request == requests.end()) {
        const mode_t *mode = current_mode(*output);
        if (!mode) {
          // The caller may have set the mode without marking it current; fall
          // back to the id it asked for.
          const auto it = std::find_if(output->modes.begin(), output->modes.end(),
                                       [&](const mode_t &candidate) { return candidate.id == output->mode_id; });
          if (it == output->modes.end()) {
            error = "output " + output->name + " has no usable mode";
            return false;
          }
          mode = &*it;
        }

        crtc_request_t fresh;
        fresh.outputs.push_back(output->name);
        fresh.mode_id = mode->id;
        fresh.width = mode->width;
        fresh.height = mode->height;
        fresh.x = output->x;
        fresh.y = output->y;
        fresh.rotation = output->rotation;
        fresh.reflect_x = output->reflect_x;
        fresh.reflect_y = output->reflect_y;
        requests.push_back(std::move(fresh));
        request = std::prev(requests.end());
      } else {
        // Mirror members share the crtc, so they share the timing; a member that
        // asks for a different size or rate cannot be honoured and the plan says
        // so instead of letting the server pick one.
        const mode_t *mode = current_mode(*output);
        if (mode && (mode->width != request->width || mode->height != request->height)) {
          error = "output " + output->name + " mirrors " + request->outputs.front() +
                  " but requests " + std::to_string(mode->width) + "x" + std::to_string(mode->height) +
                  " instead of " + std::to_string(request->width) + "x" + std::to_string(request->height);
          return false;
        }
        if (std::find(request->outputs.begin(), request->outputs.end(), output->name) == request->outputs.end()) {
          request->outputs.push_back(output->name);
        }
      }

      if (output->primary) {
        if (!primary.empty() && primary != request->outputs.front()) {
          error = "more than one output is marked primary (" + primary + ", " + output->name + ")";
          return false;
        }
        primary = request->outputs.front();
      }
    }

    int width = 0;
    int height = 0;
    for (const auto &request : requests) {
      width = std::max(width, request.x + request.width);
      height = std::max(height, request.y + request.height);
    }
    // RandR rejects a zero-sized screen, and it also rejects a rectangle that
    // does not fit the screen, so the screen is at least one pixel in each
    // direction even when nothing is enabled.
    out.screen_width = std::max(width, 1);
    out.screen_height = std::max(height, 1);
    out.crtcs = std::move(requests);
    out.primary = primary;
    return true;
  }

  bool
  apply(const state_t &state, std::string &error) {
    plan_t plan;
    if (!plan_for(state, plan, error)) {
      return false;
    }

    std::lock_guard lock(xlib_mutex);

    display_handle_t handle;
    if (!handle.open() || !randr_usable(handle.display)) {
      error = "no X display with RandR 1.2";
      return false;
    }

    Display *display = handle.display;
    const Window root = DefaultRootWindow(display);
    XRRScreenResources *resources = XRRGetScreenResourcesCurrent(display, root);
    if (!resources) {
      error = "XRRGetScreenResourcesCurrent failed";
      return false;
    }

    // Resolve the plan against the live server: mode timings, output ids and the
    // crtcs that are free to take a group.
    struct live_request_t {
      const crtc_request_t *plan = nullptr;
      RRMode mode = None;
      std::vector<RROutput> outputs;
      RRCrtc crtc = None;
    };

    std::vector<live_request_t> live;
    std::string resolve_error;

    for (const auto &request : plan.crtcs) {
      live_request_t entry;
      entry.plan = &request;

      for (const auto &name : request.outputs) {
        bool found = false;
        for (int i = 0; i < resources->noutput && !found; ++i) {
          XRROutputInfo *info = XRRGetOutputInfo(display, resources, resources->outputs[i]);
          if (!info) {
            continue;
          }
          if (name == (info->name ? info->name : "")) {
            entry.outputs.push_back(resources->outputs[i]);
            found = true;
          }
          XRRFreeOutputInfo(info);
        }
        if (!found) {
          resolve_error = "output " + name + " disappeared";
        }
      }
      if (!resolve_error.empty()) {
        break;
      }

      for (int i = 0; i < resources->nmode; ++i) {
        const auto &mode_info = resources->modes[i];
        const double refresh = refresh_from_timings(mode_info.dotClock, mode_info.hTotal, mode_info.vTotal);
        if (static_cast<int>(mode_info.width) == request.width &&
            static_cast<int>(mode_info.height) == request.height &&
            format_mode_id(request.width, request.height, refresh) == request.mode_id) {
          entry.mode = mode_info.id;
          break;
        }
      }
      if (entry.mode == None) {
        resolve_error = "mode " + request.mode_id + " is not available";
        break;
      }

      live.push_back(std::move(entry));
    }

    if (!resolve_error.empty()) {
      XRRFreeScreenResources(resources);
      error = resolve_error;
      return false;
    }

    // Snapshot everything the write touches, so a rejected request can be undone.
    screen_snapshot_t screen;
    screen.width = DisplayWidth(display, DefaultScreen(display));
    screen.height = DisplayHeight(display, DefaultScreen(display));
    screen.width_mm = DisplayWidthMM(display, DefaultScreen(display));
    screen.height_mm = DisplayHeightMM(display, DefaultScreen(display));

    std::vector<crtc_snapshot_t> snapshots;
    std::vector<RRCrtc> order;
    for (int i = 0; i < resources->ncrtc; ++i) {
      order.push_back(resources->crtcs[i]);
    }

    for (const auto crtc_id : order) {
      XRRCrtcInfo *info = XRRGetCrtcInfo(display, resources, crtc_id);
      if (!info) {
        continue;
      }
      crtc_snapshot_t snapshot;
      snapshot.crtc = crtc_id;
      snapshot.mode = info->mode;
      snapshot.enabled = info->mode != None;
      snapshot.x = static_cast<int>(info->x);
      snapshot.y = static_cast<int>(info->y);
      snapshot.rotation = info->rotation;
      for (int i = 0; i < info->noutput; ++i) {
        snapshot.outputs.push_back(info->outputs[i]);
      }
      snapshots.push_back(snapshot);
      XRRFreeCrtcInfo(info);
    }

    // Give each group a crtc: prefer one that already drives one of its outputs,
    // then one that is not claimed by another group.
    std::vector<RRCrtc> claimed;
    for (auto &entry : live) {
      for (const auto &snapshot : snapshots) {
        const bool drives_member = std::any_of(entry.outputs.begin(), entry.outputs.end(),
                                              [&](RROutput output) {
                                                return std::find(snapshot.outputs.begin(),
                                                                 snapshot.outputs.end(),
                                                                 output) != snapshot.outputs.end();
                                              });
        if (!drives_member) {
          continue;
        }
        if (std::find(claimed.begin(), claimed.end(), snapshot.crtc) != claimed.end()) {
          continue;
        }
        entry.crtc = snapshot.crtc;
        break;
      }
      if (entry.crtc == None) {
        for (const auto &snapshot : snapshots) {
          if (std::find(claimed.begin(), claimed.end(), snapshot.crtc) != claimed.end()) {
            continue;
          }
          entry.crtc = snapshot.crtc;
          break;
        }
      }
      if (entry.crtc == None) {
        XRRFreeScreenResources(resources);
        error = "no free CRTC for output " + entry.plan->outputs.front();
        return false;
      }
      claimed.push_back(entry.crtc);
    }

    const auto previous_handler = XSetErrorHandler(record_xlib_error);
    xlib_error_code = 0;

    bool ok = true;
    std::string failure;

    auto set_crtc = [&](RRCrtc crtc, RRMode mode, Rotation rotation, int x, int y,
                        const std::vector<RROutput> &outputs) {
      std::vector<RROutput> ids = outputs;
      const Status status = XRRSetCrtcConfig(display, resources, crtc, CurrentTime, x, y, mode, rotation,
                                            ids.empty() ? nullptr : ids.data(), static_cast<int>(ids.size()));
      if (status != RRSetConfigSuccess && ok) {
        ok = false;
        failure = "XRRSetCrtcConfig was rejected";
      }
    };

    XGrabServer(display);

    // Growing the screen has to happen before the crtcs that need the room;
    // shrinking has to wait until they have moved.
    const bool growing = plan.screen_width > screen.width || plan.screen_height > screen.height;
    const int screen_width = std::max(plan.screen_width, screen.width);
    const int screen_height = std::max(plan.screen_height, screen.height);
    if (growing) {
      XRRSetScreenSize(display, root, screen_width, screen_height,
                       millimetres_for(screen_width), millimetres_for(screen_height));
    }

    // Pass one: drop every crtc whose output set changes. An output cannot be on
    // two crtcs, so a group moving to a different crtc needs its old one cleared
    // first.
    for (const auto &snapshot : snapshots) {
      const auto entry = std::find_if(live.begin(), live.end(),
                                     [&](const live_request_t &candidate) { return candidate.crtc == snapshot.crtc; });
      if (entry != live.end()) {
        const bool same_outputs = [&] {
          if (entry->outputs.size() != snapshot.outputs.size()) {
            return false;
          }
          for (const auto output : entry->outputs) {
            if (std::find(snapshot.outputs.begin(), snapshot.outputs.end(), output) == snapshot.outputs.end()) {
              return false;
            }
          }
          return true;
        }();
        if (same_outputs && snapshot.enabled) {
          continue;
        }
      }
      if (snapshot.enabled) {
        set_crtc(snapshot.crtc, None, RR_Rotate_0, 0, 0, {});
      }
    }

    // Pass two: apply the plan.
    for (const auto &entry : live) {
      if (!ok) {
        break;
      }
      set_crtc(entry.crtc, entry.mode,
               rotation_from_turns(entry.plan->rotation, entry.plan->reflect_x, entry.plan->reflect_y),
               entry.plan->x, entry.plan->y, entry.outputs);
    }

    if (!ok) {
      // Undo in reverse: clear what we may have set, then restore the snapshot.
      for (const auto &entry : live) {
        set_crtc(entry.crtc, None, RR_Rotate_0, 0, 0, {});
      }
      ok = true;
      failure.clear();
      for (const auto &snapshot : snapshots) {
        if (!snapshot.enabled) {
          continue;
        }
        set_crtc(snapshot.crtc, snapshot.mode, snapshot.rotation,
                 snapshot.x, snapshot.y, snapshot.outputs);
      }
    }

    if (!growing) {
      XRRSetScreenSize(display, root, plan.screen_width, plan.screen_height,
                       millimetres_for(plan.screen_width), millimetres_for(plan.screen_height));
    }

    XUngrabServer(display);
    XSync(display, False);
    XSetErrorHandler(previous_handler);

    const int xlib_error = xlib_error_code;
    xlib_error_code = 0;

    XRRFreeScreenResources(resources);

    if (!ok || xlib_error) {
      error = failure.empty() ? "the X server rejected the configuration" : failure;
      return false;
    }
    return true;
  }

}  // namespace platf::xrandr
