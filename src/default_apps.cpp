/**
 * @file src/default_apps.cpp
 * @brief The file half of the default application entries (see default_apps.h).
 */
// lib includes
#include <boost/property_tree/ptree.hpp>

// local includes
#include "default_apps.h"
#include "file_handler.h"
#include "logging.h"

namespace default_apps {
  namespace pt = boost::property_tree;

  void
  migrate_file(const std::string &file_name, std::string_view locale) {
    try {
      pt::ptree tree;
      file_handler::read_json(file_name, tree);
      const auto offered = offer_big_screen(tree, locale_is_zh(locale));
      const auto marked = stamp_defaults(tree);
      // the names follow the language from the start too: a host whose language was set before the entries were marked never changes it again
      const auto renamed = retitle(tree, locale);
      if (!offered && !marked && !renamed) {
        return;
      }
      file_handler::write_json(file_name, tree);
      if (offered) {
        BOOST_LOG(info) << "Default apps: offered ["sv << big_screen_id << "] once; delete it from the Applications tab and it stays deleted"sv;
      }
      if (marked) {
        BOOST_LOG(info) << "Default apps: marked the default entries (names follow the language until they are renamed)"sv;
      }
      if (renamed) {
        BOOST_LOG(info) << "Default apps: names shown in ["sv << locale << "]"sv;
      }
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "Default apps: could not check ["sv << file_name << "]: "sv << e.what();
    }
  }

  bool
  retitle_file(const std::string &file_name, std::string_view locale) {
    try {
      pt::ptree tree;
      file_handler::read_json(file_name, tree);
      const auto marked = stamp_defaults(tree);
      const auto renamed = retitle(tree, locale);
      if (!marked && !renamed) {
        return false;
      }
      file_handler::write_json(file_name, tree);
      BOOST_LOG(info) << "Default apps: names shown in ["sv << locale << ']';
      return true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "Default apps: could not change the names in ["sv << file_name << "]: "sv << e.what();
      return false;
    }
  }
}  // namespace default_apps
