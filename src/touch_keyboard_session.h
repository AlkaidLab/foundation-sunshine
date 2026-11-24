/**
 * @file src/touch_keyboard_session.h
 * @brief Session-scoped Windows touch keyboard auto-invoke settings.
 */
#pragma once

#include <memory>

namespace touch_kb {

/**
 * Acquire the client preference for the lifetime of a launch ticket and its
 * stream. 0 requests suppression, 1 requests enablement, and other values
 * leave the user preference alone. The first active preference owns the
 * shared desktop; if it ends, the next active preference takes over.
 *
 * The preference is published to the GUI for the matching Windows session.
 * A null result means no preference or that publication failed; the stream
 * must continue in either case.
 */
std::shared_ptr<void>
acquire(int intent) noexcept;

}  // namespace touch_kb
