/**
 * @file src/confighttp/apps.h
 * @brief Route registration for apps.
 */
#pragma once

#include <string_view>

#include "src/confighttp/types.h"

namespace confighttp::apps {
  void
  register_routes(confighttp::https_server_t &server);

  /// Shows the names of the default entries (Desktop, Steam Big Picture, Xbox Game, Big Screen) in a language; the language of the host was changed.
  /// A name the person changed stays. Refreshes the application list when a name changed.
  void
  retitle_defaults(std::string_view locale);
}  // namespace confighttp::apps
