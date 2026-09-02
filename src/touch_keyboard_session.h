/**
 * @file src/touch_keyboard_session.h
 * @brief Session-scoped enablement of the Windows touch keyboard
 *        auto-invoke registry key group.
 *
 * When a client declares a touch-keyboard intent (or its server profile
 * opts in), the host writes the TabletTip AutoInvoke registry key group
 * for the duration of the stream session and restores the previous values
 * when the session ends.  A full-variable-isolation ablation on the host
 * (2026-09-02) confirmed the keyboard auto-invocation depends on these
 * values; no public API or Settings surface exists to toggle them
 * programmatically.
 *
 * Non-Windows platforms are no-ops.
 */
#pragma once

namespace touch_kb {

/**
 * Apply the AutoInvoke key group for the active console user.
 *
 * @param effective when false, this is a no-op (client did not opt in).
 * @return true when the key group was written (or already in the desired
 *         state); false when the write failed or the platform is
 *         non-Windows.
 */
bool
start_session(bool effective);

/**
 * Restore the values captured by start_session().  Safe to call even when
 * start_session() was skipped or failed; a missing undo file is a no-op.
 */
void
end_session();

}  // namespace touch_kb
