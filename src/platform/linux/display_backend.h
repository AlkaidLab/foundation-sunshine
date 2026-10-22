/**
 * @file src/platform/linux/display_backend.h
 * @brief Which display backend owns the session.
 *
 * Five backends can answer for the same session, and two of them are reachable
 * from sessions they do not own: XWayland answers RandR reads inside *every*
 * Wayland session, and KScreen answers whenever kscreen-doctor exists even
 * though a wlroots compositor ignores it. The precedence is therefore part of
 * the contract rather than an implementation detail -- asking RandR first would
 * silently park a wlroots session on a read-only mirror of the real outputs.
 *
 * Deliberate order:
 *   1. niri       -- own IPC socket, cannot be confused with anything else
 *   2. Mutter     -- owns a bus name only GNOME publishes
 *   3. wlroots    -- the output manager that actually owns the connectors in a
 *                    sway/Hyprland session, ahead of the XWayland RandR mirror
 *   4. xrandr     -- the only display API a bare X11 session has left
 *   5. kscreen    -- Plasma, and the floor: if no probe claimed the session,
 *                    KScreen is who gets asked at use time
 *
 * Header-only and lazy: the predicate is called in precedence order and never
 * for a backend after the winner, so deciding costs exactly what asking the
 * individual helpers one by one used to cost. Because it needs no compositor,
 * the order itself is unit-tested (`tests/unit/test_display_backend.cpp`)
 * instead of living in the call graph of display_device.cpp.
 */
#pragma once

namespace platf {
  enum class display_backend_e {
    niri,
    mutter,
    wlr_randr,
    xrandr,
    kscreen,
  };

  /**
   * @brief Pick the backend whose probe accepts the session.
   * @param supported Called with each candidate in precedence order; return
   *                  whether that backend claims this session.
   * @return The first backend that claimed it, or KScreen if none did.
   */
  template<typename Fn>
  display_backend_e
  select_display_backend(Fn &&supported) {
    if (supported(display_backend_e::niri)) {
      return display_backend_e::niri;
    }
    if (supported(display_backend_e::mutter)) {
      return display_backend_e::mutter;
    }
    if (supported(display_backend_e::wlr_randr)) {
      return display_backend_e::wlr_randr;
    }
    if (supported(display_backend_e::xrandr)) {
      return display_backend_e::xrandr;
    }
    return display_backend_e::kscreen;
  }
}  // namespace platf
