/**
 * @file src/app_scan/libraries.cpp
 * @brief Additional PC game libraries for the host scan.
 *
 * Steam, Epic and GOG stay in scanner.cpp. Launcher layouts are recorded in
 * the delivery report rather than the platform source index. The package
 * full-name grammar is Name_Version_Architecture_ResourceId_PublisherId.
 */
#include "src/app_scan/scanner.h"
#include "src/sparxie/region.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#ifdef _WIN32
  #include <windows.h>
#endif
namespace app_scan {
  namespace {
    using json = nlohmann::json;
    char
    sep() {
      return '\\';
    }
    const std::string kPublisher = "0123456789abcdefghjkmnpqrstvwxyz";
    std::string
    join_key(std::initializer_list<std::string_view> parts) {
      std::string out;
      for (const std::string_view part : parts) {
        if (!out.empty()) out.push_back(sep());
        out.append(part);
      }
      return out;
    }
    std::string
    lower_copy(std::string_view value) {
      std::string result(value);
      std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return result;
    }
    std::string
    trim_copy(std::string_view value) {
      size_t begin = 0;
      size_t end = value.size();
      while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
      while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
      return std::string(value.substr(begin, end - begin));
    }
    bool
    wanted(const ScanRequest &request, Source source) {
      return std::find(request.sources.begin(), request.sources.end(), source) != request.sources.end();
    }
    bool
    refuse_non_windows(const HostContext &context, Source source, ScanResult &result) {
      if (context.platform == HostPlatform::windows_host) return false;
      result.warnings.emplace_back(std::string(source_name(source)) + ".unsupported");
      return true;
    }
    bool
    is_dir(const fs::path &path) {
      std::error_code ec;
      return fs::is_directory(path, ec);
    }
    bool
    is_file(const fs::path &path) {
      std::error_code ec;
      return fs::is_regular_file(path, ec);
    }
    std::optional<std::string>
    read_file(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      if (!in) return std::nullopt;
      return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    std::string
    quote_cmd(const std::string &path) {
      if (path.empty() || path.front() == char(34)) return path;
      return std::string(1, char(34)) + path + std::string(1, char(34));
    }
    std::string
    norm_key(std::string path) {
      for (char &c : path) {
        if (c == char(47))
          c = sep();
        else
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      while (path.size() > 3 && path.back() == sep()) path.pop_back();
      return path;
    }
    bool
    related(const std::string &left, const std::string &right) {
      if (left.empty() || right.empty()) return false;
      const std::string a = norm_key(left);
      const std::string b = norm_key(right);
      if (a == b) return true;
      const std::string &shorter = a.size() < b.size() ? a : b;
      const std::string &longer = a.size() < b.size() ? b : a;
      return longer.starts_with(shorter) && longer.size() > shorter.size() && longer[shorter.size()] == sep();
    }
    std::optional<std::string>
    reg_get(const HostContext &context, const std::string &key, const std::string &value, bool user) {
      if (!context.registry) return std::nullopt;
      if (user) {
        if (auto found = context.registry->read_user_string(key, value)) return found;
      }
      return context.registry->read_string(key, value);
    }
    std::vector<std::string>
    reg_kids(const HostContext &context, const std::string &key, bool user) {
      if (!context.registry) return {};
      if (user) {
        const auto kids = context.registry->user_subkeys(key);
        if (!kids.empty()) return kids;
      }
      return context.registry->subkeys(key);
    }
    std::string
    utf16le_to_utf8(std::string_view bytes) {
      std::string out;
      for (size_t index = 0; index + 1 < bytes.size(); index += 2) {
        const unsigned unit = static_cast<unsigned char>(bytes[index]) | (static_cast<unsigned char>(bytes[index + 1]) << 8);
        if (unit == 0) break;
        if (unit < 0x80)
          out.push_back(static_cast<char>(unit));
        else if (unit < 0x800) {
          out.push_back(static_cast<char>(0xC0 | (unit >> 6)));
          out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
        }
        else {
          out.push_back(static_cast<char>(0xE0 | (unit >> 12)));
          out.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
        }
      }
      return out;
    }
    bool
    publisher_id(std::string_view text) {
      if (text.size() != 13) return false;
      for (unsigned char c : text) {
        if (kPublisher.find(static_cast<char>(std::tolower(c))) == std::string::npos) return false;
      }
      return true;
    }
    bool
    version_token(std::string_view text) {
      int dots = 0;
      int run = 0;
      if (text.empty()) return false;
      for (unsigned char c : text) {
        if (c == char(46)) {
          if (run == 0) return false;
          ++dots;
          run = 0;
          continue;
        }
        if (c < char(48) || c > char(57)) return false;
        ++run;
      }
      return dots == 3 && run > 0;
    }
    std::vector<fs::path>
    drive_roots(const HostContext &context) {
      if (!context.drive_roots.empty()) return context.drive_roots;
#ifdef _WIN32
      const DWORD needed = GetLogicalDriveStringsW(0, nullptr);
      if (needed == 0) return {};
      std::wstring buffer(needed, wchar_t {});
      if (GetLogicalDriveStringsW(needed, buffer.data()) == 0) return {};
      std::vector<fs::path> roots;
      for (size_t index = 0; index < buffer.size() && buffer[index];) {
        std::wstring item;
        while (index < buffer.size() && buffer[index]) item.push_back(buffer[index++]);
        ++index;
        if (!item.empty()) roots.emplace_back(item);
      }
      return roots;
#else
      return {};
#endif
    }
    std::optional<std::string>
    first_image(const fs::path &dir) {
      if (!is_dir(dir)) return std::nullopt;
      std::error_code ec;
      for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = lower_copy(it->path().extension().string());
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".ico") return to_utf8(it->path());
      }
      return std::nullopt;
    }
    std::string
    cached_cover(const HostContext &context, const std::string &install_id) {
      if (!context.covers_dir || install_id.empty() || !is_dir(*context.covers_dir)) return {};
      const std::string prefix = "sparxie-" + install_id + "-";
      std::string best;
      std::error_code ec;
      for (fs::directory_iterator it(*context.covers_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = it->path().filename().string();
        if (!name.starts_with(prefix) || !lower_copy(name).ends_with(".png")) continue;
        if (name > best) best = name;
      }
      return best.empty() ? std::string {} : to_utf8(*context.covers_dir / best);
    }
    std::vector<std::string>
    xml_texts(std::string_view xml, std::string_view tag) {
      std::vector<std::string> values;
      const std::string open = "<" + std::string(tag);
      const std::string close = "</" + std::string(tag) + ">";
      size_t offset = 0;
      while (offset < xml.size()) {
        const size_t at = xml.find(open, offset);
        if (at == std::string_view::npos) break;
        const size_t after = at + open.size();
        if (after >= xml.size() || (xml[after] != char(62) && !std::isspace(static_cast<unsigned char>(xml[after])))) {
          offset = after;
          continue;
        }
        const size_t end = xml.find(char(62), after);
        if (end == std::string_view::npos) break;
        const size_t stop = xml.find(close, end);
        if (stop == std::string_view::npos) break;
        values.push_back(trim_copy(xml.substr(end + 1, stop - end - 1)));
        offset = stop + close.size();
      }
      return values;
    }
    std::optional<std::string>
    xml_attr(std::string_view xml, std::string_view element, std::string_view attr) {
      const std::string open = "<" + std::string(element);
      size_t offset = 0;
      while (offset < xml.size()) {
        const size_t at = xml.find(open, offset);
        if (at == std::string_view::npos) return std::nullopt;
        const size_t after = at + open.size();
        if (after < xml.size() && xml[after] != char(62) && !std::isspace(static_cast<unsigned char>(xml[after]))) {
          offset = after;
          continue;
        }
        const size_t end = xml.find(char(62), after);
        if (end == std::string_view::npos) return std::nullopt;
        const std::string tag(xml.substr(at, end - at));
        const std::string key = std::string(attr) + "=";
        const size_t found = tag.find(key);
        if (found == std::string::npos) {
          offset = end + 1;
          continue;
        }
        size_t value_at = found + key.size();
        if (value_at >= tag.size()) return std::nullopt;
        const char quote = tag[value_at];
        if (quote != char(34) && quote != char(39)) return std::nullopt;
        const size_t finish = tag.find(quote, value_at + 1);
        if (finish == std::string::npos) return std::nullopt;
        return tag.substr(value_at + 1, finish - value_at - 1);
      }
      return std::nullopt;
    }
    std::optional<std::string>
    gaming_folder(std::string_view bytes) {
      std::string text;
      if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE)
        text = utf16le_to_utf8(bytes.substr(2));
      else if (bytes.size() >= 4 && bytes[1] == 0)
        text = utf16le_to_utf8(bytes);
      else
        text = std::string(bytes);
      text.erase(std::remove(text.begin(), text.end(), char(0)), text.end());
      const size_t line = text.find_first_of("\r\n");
      if (line != std::string::npos) text.resize(line);
      text = trim_copy(text);
      if (text.find("..") != std::string::npos) return std::nullopt;
      while (!text.empty() && (text.back() == sep() || text.back() == char(47))) text.pop_back();
      if (text.empty()) return std::nullopt;
      return text;
    }
    struct XmlGame {
      std::string name;
      std::string exe;
      std::string app_id;
      std::string identity;
      std::string logo;
    };
    std::optional<XmlGame>
    parse_game_config(std::string_view xml) {
      const auto name = xml_attr(xml, "ShellVisuals", "DefaultDisplayName");
      const auto exe = xml_attr(xml, "Executable", "Name");
      const auto identity = xml_attr(xml, "Identity", "Name");
      if (!exe || exe->empty()) return std::nullopt;
      XmlGame game;
      game.name = name.value_or(std::string {});
      game.exe = *exe;
      game.app_id = xml_attr(xml, "Executable", "Id").value_or("Game");
      game.identity = identity.value_or(std::string {});
      game.logo = xml_attr(xml, "ShellVisuals", "Square150x150Logo").value_or(xml_attr(xml, "ShellVisuals", "StoreLogo").value_or(std::string {}));
      return game;
    }
    std::string
    family_for(const HostContext &context, const fs::path &content, const std::string &identity) {
      const std::string content_text = to_utf8(content);
      if (context.packages) {
        for (const PackageRecord &package : context.packages->packages()) {
          if (related(package.install_path, content_text) && !package.family_name.empty()) return package.family_name;
        }
      }
      for (const fs::path &root : context.package_roots) {
        if (!is_dir(root)) continue;
        std::error_code ec;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
          if (!it->is_directory(ec)) continue;
          const auto family = package_family_from_full_name(it->path().filename().string());
          if (!family) continue;
          if (!identity.empty() && family->starts_with(identity + "_")) return *family;
          if (related(to_utf8(it->path()), content_text)) return *family;
        }
      }
      return {};
    }
    void
    scan_xbox(const HostContext &context, ScanResult &result) {
      if (refuse_non_windows(context, Source::xbox, result)) return;
      bool marker = false;
      size_t found = 0;
      for (const fs::path &root : drive_roots(context)) {
        const auto bytes = read_file(root / ".GamingRoot");
        if (!bytes) continue;
        const auto folder = gaming_folder(*bytes);
        if (!folder) continue;
        marker = true;
        fs::path library = folder->find(char(58)) == std::string::npos ? root / from_utf8(*folder) : from_utf8(*folder);
        if (!is_dir(library)) continue;
        std::error_code ec;
        for (fs::directory_iterator game_it(library, ec), game_end; !ec && game_it != game_end; game_it.increment(ec)) {
          if (!game_it->is_directory(ec)) continue;
          const fs::path candidates[] = { game_it->path() / "Content" / "MicrosoftGame.config", game_it->path() / "MicrosoftGame.config" };
          for (const fs::path &config_path : candidates) {
            const auto xml = read_file(config_path);
            if (!xml) continue;
            const auto parsed = parse_game_config(*xml);
            if (!parsed) continue;
            const fs::path content = config_path.parent_path();
            const fs::path exe = content / from_utf8(parsed->exe);
            Game game;
            game.platform = "xbox";
            game.sources = { "xbox" };
            game.name = parsed->name.empty() || parsed->name.starts_with("ms-resource:") ? game_it->path().filename().string() : parsed->name;
            game.id = parsed->identity.empty() ? game.name : parsed->identity;
            game.install_dir = to_utf8(content);
            game.exe = to_utf8(exe);
            const std::string family = family_for(context, content, parsed->identity);
            if (!family.empty())
              game.launch = std::string("shell:AppsFolder") + sep() + family + "!" + parsed->app_id;
            else
              game.launch = quote_cmd(game.exe);
            if (!parsed->logo.empty()) {
              const fs::path logo = content / from_utf8(parsed->logo);
              if (is_file(logo)) game.cover_hint = to_utf8(logo);
            }
            result.games.push_back(std::move(game));
            ++found;
            break;
          }
        }
      }
      if (!marker)
        result.warnings.emplace_back("xbox.not_found");
      else if (found == 0)
        result.warnings.emplace_back("xbox.empty");
    }
    std::vector<std::string>
    content_ids(std::string_view xml) {
      auto ids = xml_texts(xml, "contentID");
      if (ids.empty()) ids = xml_texts(xml, "contentId");
      return ids;
    }
    void
    add_ea_dir(const fs::path &dir, ScanResult &result, size_t &found) {
      const auto xml = read_file(dir / "__Installer" / "installerdata.xml");
      if (!xml) return;
      const auto ids = content_ids(*xml);
      if (ids.empty()) return;
      const auto titles = xml_texts(*xml, "gameTitle");
      Game game;
      game.platform = "ea";
      game.sources = { "ea" };
      game.name = titles.empty() || titles.front().empty() ? dir.filename().string() : titles.front();
      game.id = ids.front();
      game.install_dir = to_utf8(dir);
      game.launch = "link2ea://launchgame/" + ids.front();
      result.games.push_back(std::move(game));
      ++found;
    }
    void
    scan_ea(const HostContext &context, ScanResult &result) {
      if (refuse_non_windows(context, Source::ea, result)) return;
      std::vector<fs::path> libraries;
      bool saw = false;
      if (context.local_app_data) {
        const fs::path settings = *context.local_app_data / "Electronic Arts" / "EA Desktop";
        if (is_dir(settings)) {
          saw = true;
          std::error_code ec;
          for (fs::directory_iterator it(settings, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = lower_copy(it->path().filename().string());
            if (!name.starts_with("user_") || !name.ends_with(".ini")) continue;
            const auto text = read_file(it->path());
            if (!text) continue;
            for (size_t offset = 0; offset < text->size();) {
              size_t line_end = text->find(char(10), offset);
              if (line_end == std::string::npos) line_end = text->size();
              std::string line = trim_copy(std::string_view(*text).substr(offset, line_end - offset));
              offset = line_end + 1;
              const std::string key = "user.downloadinplacedir=";
              if (!lower_copy(line).starts_with(key)) continue;
              const std::string path = trim_copy(std::string_view(line).substr(key.size()));
              if (!path.empty()) libraries.push_back(from_utf8(path));
            }
          }
        }
      }
      const std::string keys[] = {
        join_key({ "SOFTWARE", "WOW6432Node", "EA Games" }),
        join_key({ "SOFTWARE", "EA Games" }),
        join_key({ "SOFTWARE", "WOW6432Node", "Origin Games" }),
        join_key({ "SOFTWARE", "Origin Games" }),
      };
      for (const std::string &key : keys) {
        const auto ids = reg_kids(context, key, false);
        if (!ids.empty()) saw = true;
        for (const std::string &id : ids) {
          const std::string child = key + sep() + id;
          auto path = reg_get(context, child, "Install Dir", false);
          if (!path) path = reg_get(context, child, "InstallDir", false);
          if (!path) path = reg_get(context, child, "path", false);
          if (path && !path->empty()) libraries.push_back(from_utf8(*path));
        }
      }
      size_t found = 0;
      std::set<std::string> seen;
      for (const fs::path &library : libraries) {
        if (!is_dir(library)) continue;
        saw = true;
        if (is_file(library / "__Installer" / "installerdata.xml")) {
          const std::string key = norm_key(to_utf8(library));
          if (seen.insert(key).second) add_ea_dir(library, result, found);
          continue;
        }
        std::error_code ec;
        for (fs::directory_iterator it(library, ec), end; !ec && it != end; it.increment(ec)) {
          if (!it->is_directory(ec)) continue;
          const std::string key = norm_key(to_utf8(it->path()));
          if (!seen.insert(key).second) continue;
          add_ea_dir(it->path(), result, found);
        }
      }
      if (!saw)
        result.warnings.emplace_back("ea.not_found");
      else if (found == 0)
        result.warnings.emplace_back("ea.empty");
    }
    void
    scan_ubisoft(const HostContext &context, ScanResult &result) {
      if (refuse_non_windows(context, Source::ubisoft, result)) return;
      const std::string key = join_key({ "SOFTWARE", "WOW6432Node", "Ubisoft", "Launcher", "Installs" });
      const auto ids = reg_kids(context, key, false);
      if (ids.empty()) {
        result.warnings.emplace_back("ubisoft.not_found");
        return;
      }
      size_t found = 0;
      for (const std::string &id : ids) {
        const auto path = reg_get(context, key + sep() + id, "InstallDir", false);
        if (!path || path->empty()) continue;
        auto name = reg_get(context, key + sep() + id, "DisplayName", false);
        if (!name) name = reg_get(context, key + sep() + id, "Name", false);
        Game game;
        game.platform = "ubisoft";
        game.sources = { "ubisoft" };
        game.id = id;
        game.install_dir = *path;
        game.name = name && !name->empty() ? *name : from_utf8(*path).filename().string();
        game.launch = "uplay://launch/" + id + "/0";
        if (context.local_app_data) {
          if (auto image = first_image(*context.local_app_data / "Ubisoft Game Launcher" / "cache" / "assets" / id)) game.cover_hint = *image;
        }
        result.games.push_back(std::move(game));
        ++found;
      }
      if (found == 0) result.warnings.emplace_back("ubisoft.empty");
    }
    std::string
    bnet_name(const std::string &code, const fs::path &dir) {
      const std::string key = lower_copy(code);
      if (key == "wow" || key == "osi") return "World of Warcraft";
      if (key == "s2") return "StarCraft II";
      if (key == "s1") return "StarCraft Remastered";
      if (key == "w3") return "Warcraft III";
      if (key == "d3") return "Diablo III";
      if (key == "hero") return "Heroes of the Storm";
      if (key == "hsb") return "Hearthstone";
      if (key == "pro") return "Overwatch";
      if (key == "fenris") return "Diablo IV";
      if (key == "dst2") return "Destiny 2";
      if (key == "anbs" || key == "rtro") return "Diablo Immortal";
      if (key == "wlby") return "Warcraft Rumble";
      if (key == "viper" || key == "odin" || key == "lazr" || key == "zeus" || key == "fore" || key == "auks") return "Call of Duty";
      const std::string folder = dir.filename().string();
      return folder.empty() ? code : folder;
    }
    bool
    bnet_code(const std::string &text) {
      const std::string key = lower_copy(text);
      static const char *codes[] = { "wow", "osi", "s2", "s1", "w3", "d3", "hero", "hsb", "pro", "fenris", "dst2", "anbs", "rtro", "wlby", "viper", "odin", "lazr", "zeus", "fore", "auks", nullptr };
      for (const char **code = codes; *code; ++code)
        if (key == *code) return true;
      return false;
    }
    bool
    looks_like_path(const std::string &text) {
      if (text.size() < 3) return false;
      const bool slash = text.find(sep()) != std::string::npos || text.find(char(47)) != std::string::npos;
      if (text.find(char(58)) != std::string::npos && slash) return true;
      return text.front() == char(47) || text.front() == sep();
    }
    std::vector<std::string>
    ascii_strings(std::string_view bytes) {
      std::vector<std::string> values;
      std::string current;
      auto flush = [&] {
        if (current.size() >= 2) values.push_back(current);
        current.clear();
      };
      for (unsigned char c : bytes) {
        if (c >= 32 && c < 127)
          current.push_back(static_cast<char>(c));
        else
          flush();
      }
      flush();
      return values;
    }
    void
    add_bnet(const std::string &code, const std::string &path, ScanResult &result, std::set<std::string> &seen) {
      if (!bnet_code(code) || !looks_like_path(path)) return;
      const fs::path dir = from_utf8(path);
      if (!is_dir(dir)) return;
      if (!seen.insert(lower_copy(code) + "|" + norm_key(path)).second) return;
      Game game;
      game.platform = "battlenet";
      game.sources = { "battlenet" };
      game.id = lower_copy(code);
      game.name = bnet_name(code, dir);
      game.install_dir = path;
      game.launch = "battlenet://" + game.id;
      result.games.push_back(std::move(game));
    }
    void
    scan_battlenet(const HostContext &context, ScanResult &result) {
      if (refuse_non_windows(context, Source::battlenet, result)) return;
      bool saw = false;
      const std::string install_key = join_key({ "SOFTWARE", "WOW6432Node", "Blizzard Entertainment", "Battle.net" });
      if (reg_get(context, install_key, "InstallPath", false)) saw = true;
      std::set<std::string> seen;
      if (context.program_data) {
        const fs::path agent = *context.program_data / "Battle.net" / "Agent";
        if (is_dir(agent)) saw = true;
        for (const char *name : { ".product.db", "product.db" }) {
          const auto bytes = read_file(agent / name);
          if (!bytes) continue;
          saw = true;
          const auto strings = ascii_strings(*bytes);
          for (size_t index = 0; index < strings.size(); ++index) {
            if (!bnet_code(strings[index])) continue;
            for (size_t next = index + 1; next < strings.size() && next < index + 8; ++next) {
              if (looks_like_path(strings[next])) {
                add_bnet(strings[index], strings[next], result, seen);
                break;
              }
            }
          }
        }
      }
      if (context.roaming_app_data) {
        const auto text = read_file(*context.roaming_app_data / "Battle.net" / "Battle.net.config");
        if (text) {
          saw = true;
          const json document = json::parse(text->begin(), text->end(), nullptr, false);
          if (!document.is_discarded()) {
            const std::function<void(const json &, const std::string &)> walk = [&](const json &node, const std::string &key) {
              if (!node.is_object()) return;
              std::string path;
              if (node.contains("InstallPath") && node["InstallPath"].is_string())
                path = node["InstallPath"].get<std::string>();
              else if (node.contains("install_path") && node["install_path"].is_string())
                path = node["install_path"].get<std::string>();
              if (!path.empty()) add_bnet(key, path, result, seen);
              for (auto it = node.begin(); it != node.end(); ++it) walk(it.value(), it.key());
            };
            walk(document, "");
          }
        }
      }
      const size_t found = seen.size();
      if (!saw)
        result.warnings.emplace_back("battlenet.not_found");
      else if (found == 0)
        result.warnings.emplace_back("battlenet.empty");
    }
    std::optional<std::string>
    yaml_value(std::string_view text, std::string_view key) {
      const std::string prefix = std::string(key) + ":";
      size_t offset = 0;
      while (offset < text.size()) {
        size_t line_end = text.find(char(10), offset);
        if (line_end == std::string_view::npos) line_end = text.size();
        std::string line = trim_copy(text.substr(offset, line_end - offset));
        offset = line_end + 1;
        if (!line.starts_with(prefix)) continue;
        std::string value = trim_copy(std::string_view(line).substr(prefix.size()));
        if (value.size() >= 2 && value.front() == char(34) && value.back() == char(34)) value = value.substr(1, value.size() - 2);
        if (!value.empty()) return value;
      }
      return std::nullopt;
    }
    void
    scan_riot(const HostContext &context, ScanResult &result) {
      if (refuse_non_windows(context, Source::riot, result)) return;
      if (!context.program_data) {
        result.warnings.emplace_back("riot.not_found");
        return;
      }
      const fs::path root = *context.program_data / "Riot Games";
      const auto installs = read_file(root / "RiotClientInstalls.json");
      if (!installs) {
        result.warnings.emplace_back("riot.not_found");
        return;
      }
      const json document = json::parse(installs->begin(), installs->end(), nullptr, false);
      std::string client;
      if (!document.is_discarded() && document.is_object()) {
        if (document.contains("rc_live") && document["rc_live"].is_string()) client = document["rc_live"].get<std::string>();
        if (client.empty() && document.contains("rc_default") && document["rc_default"].is_string()) client = document["rc_default"].get<std::string>();
      }
      const fs::path metadata = root / "Metadata";
      size_t found = 0;
      if (is_dir(metadata)) {
        std::error_code ec;
        for (fs::directory_iterator it(metadata, ec), end; !ec && it != end; it.increment(ec)) {
          if (!it->is_directory(ec)) continue;
          const std::string folder = it->path().filename().string();
          const size_t dot = folder.rfind(char(46));
          if (dot == std::string::npos || dot == 0) continue;
          const std::string product = folder.substr(0, dot);
          const std::string patch = folder.substr(dot + 1);
          std::optional<std::string> text;
          std::error_code inner;
          for (fs::directory_iterator file_it(it->path(), inner), file_end; !inner && file_it != file_end; file_it.increment(inner)) {
            if (!file_it->is_regular_file(inner)) continue;
            text = read_file(file_it->path());
            if (text) break;
          }
          if (!text) continue;
          const json as_json = json::parse(text->begin(), text->end(), nullptr, false);
          std::string install;
          std::string shortcut;
          if (!as_json.is_discarded() && as_json.is_object()) {
            if (as_json.contains("product_install_full_path") && as_json["product_install_full_path"].is_string()) install = as_json["product_install_full_path"].get<std::string>();
            if (as_json.contains("shortcut_name") && as_json["shortcut_name"].is_string()) shortcut = as_json["shortcut_name"].get<std::string>();
          }
          if (install.empty()) install = yaml_value(*text, "product_install_full_path").value_or(std::string {});
          if (shortcut.empty()) shortcut = yaml_value(*text, "shortcut_name").value_or(std::string {});
          if (install.empty()) continue;
          if (shortcut.size() > 4 && lower_copy(shortcut).ends_with(".lnk")) shortcut.resize(shortcut.size() - 4);
          Game game;
          game.platform = "riot";
          game.sources = { "riot" };
          game.id = product;
          game.name = shortcut.empty() ? product : shortcut;
          game.install_dir = install;
          const std::string patchline = patch.empty() ? "live" : patch;
          if (!client.empty())
            game.launch = quote_cmd(client) + " --launch-product=" + product + " --launch-patchline=" + patchline;
          else
            game.launch = " --launch-product=" + product + " --launch-patchline=" + patchline;
          result.games.push_back(std::move(game));
          ++found;
        }
      }
      if (found == 0) result.warnings.emplace_back("riot.empty");
    }
    class UserRegistry final: public sparxie::Registry {
    public:
      explicit UserRegistry(const RegistryView *view): view(view) {}
      sparxie::RegValue
      get(std::string_view key, std::string_view name) const override {
        sparxie::RegValue value;
        if (!view) return value;
        auto text = view->read_user_string(std::string(key), std::string(name));
        if (!text) text = view->read_string(std::string(key), std::string(name));
        if (!text) return value;
        value.kind = sparxie::RegValue::Kind::text;
        value.text = *text;
        return value;
      }
      bool
      set_dword(std::string_view, std::string_view, std::uint32_t) override { return false; }
      bool
      set_binary(std::string_view, std::string_view, const std::vector<std::uint8_t> &) override { return false; }

    private:
      const RegistryView *view;
    };
    std::string
    hoyo_title(const sparxie::Install &install) {
      if (install.game == "genshin" && install.region == "global") return "Genshin Impact";
      if (install.game == "genshin") return "原神";
      if (install.game == "starrail" && install.region == "global") return "Honkai: Star Rail";
      if (install.game == "starrail") return "崩坏：星穹铁道";
      if (install.game == "zzz" && install.region == "global") return "Zenless Zone Zero";
      if (install.game == "zzz") return "绝区零";
      return install.game;
    }
    void
    add_hoyo(const HostContext &context, const sparxie::Install &install, const std::string &platform, ScanResult &result) {
      Game game;
      game.platform = platform;
      game.sources = { platform };
      game.id = install.id.empty() ? install.game : install.id;
      game.name = hoyo_title(install);
      game.region = install.region;
      game.install_dir = install.dir;
      game.exe = install.exe_path;
      game.launch = quote_cmd(install.exe_path);
      game.cover_hint = cached_cover(context, game.id);
      result.games.push_back(std::move(game));
    }
    void
    scan_hoyoplay(const HostContext &context, ScanResult &result) {
      if (refuse_non_windows(context, Source::hoyoplay, result)) return;
      const std::pair<const char *, const char *> vendors[] = { { "miHoYo", "1_1" }, { "Cognosphere", "1_0" } };
      const char *biz[] = { "hk4e_cn", "hkrpg_cn", "nap_cn", "hk4e_global", "hkrpg_global", "nap_global" };
      UserRegistry registry(context.registry);
      bool saw = false;
      size_t found = 0;
      for (const auto &vendor : vendors) {
        const std::string parent = join_key({ "Software", vendor.first, "HYP" });
        if (!reg_kids(context, parent, true).empty()) saw = true;
        for (const char *one : biz) {
          const std::string key = join_key({ "Software", vendor.first, "HYP", vendor.second, one });
          const auto path = reg_get(context, key, "GameInstallPath", true);
          if (!path || path->empty()) continue;
          saw = true;
          const sparxie::IdentifyResult identified = sparxie::identify_install(from_utf8(*path), registry);
          if (!identified.status.ok) continue;
          add_hoyo(context, identified.install, "hoyoplay", result);
          ++found;
        }
      }
      if (!saw)
        result.warnings.emplace_back("hoyoplay.not_found");
      else if (found == 0)
        result.warnings.emplace_back("hoyoplay.empty");
    }
    void
    add_wegame(const json &row, ScanResult &result) {
      if (!row.is_object()) return;
      const auto text_of = [&](std::initializer_list<const char *> names) {
        for (const char *name : names) {
          if (row.contains(name) && row[name].is_string() && !row[name].get<std::string>().empty()) return row[name].get<std::string>();
        }
        return std::string {};
      };
      const std::string id = text_of({ "id", "game_id", "app_id", "appid" });
      const std::string name = text_of({ "name", "display_name", "displayName", "title" });
      const std::string exe = text_of({ "exe", "executable", "executablePath" });
      const std::string dir = text_of({ "dir", "install_dir", "installDir", "path" });
      const std::string icon = text_of({ "icon", "cover", "logo" });
      if (id.empty() && name.empty()) return;
      Game game;
      game.platform = "wegame";
      game.sources = { "wegame" };
      game.id = id.empty() ? name : id;
      game.name = name.empty() ? id : name;
      game.exe = exe;
      game.install_dir = dir.empty() && !exe.empty() ? to_utf8(from_utf8(exe).parent_path()) : dir;
      if (!id.empty())
        game.launch = "wegame://launch_game/" + id;
      else
        game.launch = quote_cmd(exe);
      if (!icon.empty() && (is_file(from_utf8(icon)) || icon.starts_with("http://") || icon.starts_with("https://"))) game.cover_hint = icon;
      result.games.push_back(std::move(game));
    }
    void
    consume_wegame_json(std::string_view text, ScanResult &result, size_t &found) {
      const json document = json::parse(text.begin(), text.end(), nullptr, false);
      if (document.is_discarded()) return;
      const size_t before = result.games.size();
      if (document.is_array()) {
        for (const auto &row : document) add_wegame(row, result);
      }
      else if (document.is_object()) {
        if (document.contains("apps") && document["apps"].is_array())
          for (const auto &row : document["apps"]) add_wegame(row, result);
        else if (document.contains("rail_apps") && document["rail_apps"].is_array())
          for (const auto &row : document["rail_apps"]) add_wegame(row, result);
        else
          add_wegame(document, result);
      }
      found += result.games.size() - before;
    }
    void
    scan_wegame(const HostContext &context, ScanResult &result) {
      if (refuse_non_windows(context, Source::wegame, result)) return;
      std::string install;
      if (auto path = reg_get(context, join_key({ "SOFTWARE", "WOW6432Node", "Tencent", "WeGame" }), "InstallPath", false)) install = *path;
      if (install.empty()) {
        if (auto path = reg_get(context, join_key({ "Software", "Tencent", "WeGame" }), "InstallPath", true)) install = *path;
      }
      std::vector<fs::path> files;
      if (!install.empty()) {
        const fs::path root = from_utf8(install);
        files.push_back(root / "rail_apps.json");
        files.push_back(root / "apps" / "rail_apps.json");
        if (is_dir(root / "rail_apps")) {
          std::error_code ec;
          for (fs::directory_iterator it(root / "rail_apps", ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec))
              files.push_back(it->path());
            else if (it->is_directory(ec))
              files.push_back(it->path() / "app.json");
          }
        }
      }
      if (context.roaming_app_data) files.push_back(*context.roaming_app_data / "Tencent" / "WeGame" / "rail_apps.json");
      if (context.local_app_data) files.push_back(*context.local_app_data / "Tencent" / "WeGame" / "rail_apps.json");
      bool saw = !install.empty();
      size_t found = 0;
      for (const fs::path &file : files) {
        const auto text = read_file(file);
        if (!text) continue;
        saw = true;
        consume_wegame_json(*text, result, found);
      }
      if (!saw)
        result.warnings.emplace_back("wegame.not_found");
      else if (found == 0)
        result.warnings.emplace_back("wegame.empty");
    }
    void
    scan_sparxie(const HostContext &context, ScanResult &result) {
      if (refuse_non_windows(context, Source::sparxie, result)) return;
      if (!context.sparxie_exe || !is_file(*context.sparxie_exe)) {
        result.warnings.emplace_back("sparxie.absent");
        return;
      }
      if (!context.sparxie_data) {
        result.warnings.emplace_back("sparxie.empty");
        return;
      }
      const auto text = read_file(*context.sparxie_data / "config.json");
      if (!text) {
        result.warnings.emplace_back("sparxie.empty");
        return;
      }
      const json document = json::parse(text->begin(), text->end(), nullptr, false);
      if (document.is_discarded() || !document.is_object() || !document.contains("profiles") || !document["profiles"].is_array()) {
        result.warnings.emplace_back("sparxie.unreadable");
        return;
      }
      UserRegistry registry(context.registry);
      size_t found = 0;
      for (const auto &profile : document["profiles"]) {
        if (!profile.is_object()) continue;
        const std::string id = profile.value("id", std::string {});
        const std::string name = profile.value("displayName", std::string {});
        const std::string exe = profile.value("executablePath", std::string {});
        if (id.empty() || exe.empty()) continue;
        Game game;
        game.platform = "sparxie";
        game.sources = { "sparxie" };
        game.id = id;
        game.profile = id;
        game.name = name.empty() ? id : name;
        game.exe = exe;
        game.install_dir = to_utf8(from_utf8(exe).parent_path());
        game.launch = quote_cmd(exe);
        const sparxie::IdentifyResult identified = sparxie::identify_install(from_utf8(exe), registry);
        if (identified.status.ok) {
          game.region = identified.install.region;
          if (!identified.install.dir.empty()) game.install_dir = identified.install.dir;
          game.cover_hint = cached_cover(context, identified.install.id);
        }
        LaunchChoice touch;
        touch.id = "touch";
        LaunchChoice direct;
        direct.id = "pc";
        direct.launch = game.launch;
        game.launches = { std::move(touch), std::move(direct) };
        result.games.push_back(std::move(game));
        ++found;
      }
      if (found == 0) result.warnings.emplace_back("sparxie.empty");
    }
    int
    source_rank(std::string_view platform) {
      static const char *order[] = { "steam", "epic", "xbox", "ea", "ubisoft", "battlenet", "riot", "gog", "wegame", "hoyoplay", "sparxie", "directory", nullptr };
      for (int index = 0; order[index]; ++index)
        if (platform == order[index]) return index;
      return 100;
    }
    void
    merge_game(Game &into, const Game &from) {
      const bool from_better = source_rank(from.platform) < source_rank(into.platform);
      if (from_better) {
        into.platform = from.platform;
        if (!from.name.empty()) into.name = from.name;
        if (!from.id.empty()) into.id = from.id;
        if (!from.launch.empty()) into.launch = from.launch;
      }
      else if (into.launch.empty())
        into.launch = from.launch;
      if (into.exe.empty()) into.exe = from.exe;
      if (into.install_dir.empty()) into.install_dir = from.install_dir;
      if (into.cover_hint.empty()) into.cover_hint = from.cover_hint;
      if (into.region.empty()) into.region = from.region;
      if (into.profile.empty()) into.profile = from.profile;
      for (const std::string &source : from.sources) {
        if (std::find(into.sources.begin(), into.sources.end(), source) == into.sources.end()) into.sources.push_back(source);
      }
      for (const LaunchChoice &choice : from.launches) {
        const auto existing = std::find_if(into.launches.begin(), into.launches.end(), [&](const LaunchChoice &item) { return item.id == choice.id; });
        if (existing == into.launches.end())
          into.launches.push_back(choice);
        else if (existing->launch.empty())
          existing->launch = choice.launch;
      }
      if (!into.sources.empty()) {
        const auto primary = std::find(into.sources.begin(), into.sources.end(), into.platform);
        if (primary != into.sources.end() && primary != into.sources.begin()) std::rotate(into.sources.begin(), primary, primary + 1);
      }
    }
  }  // namespace
  std::optional<std::string>
  package_family_from_full_name(std::string_view full_name) {
    std::vector<std::string> parts;
    size_t offset = 0;
    while (offset <= full_name.size()) {
      const size_t at = full_name.find(char(95), offset);
      parts.emplace_back(full_name.substr(offset, at == std::string_view::npos ? std::string_view::npos : at - offset));
      if (at == std::string_view::npos) break;
      offset = at + 1;
    }
    int version_at = -1;
    for (int index = 0; index < static_cast<int>(parts.size()); ++index) {
      if (version_token(parts[index])) {
        version_at = index;
        break;
      }
    }
    if (version_at <= 0 || parts.empty() || !publisher_id(parts.back())) return std::nullopt;
    std::string name;
    for (int index = 0; index < version_at; ++index) {
      if (!name.empty()) name.push_back(char(95));
      name += parts[index];
    }
    if (name.empty()) return std::nullopt;
    return name + "_" + parts.back();
  }
  void
  dedupe_games(std::vector<Game> &games) {
    const int count = static_cast<int>(games.size());
    std::vector<int> parent(count);
    for (int index = 0; index < count; ++index) {
      parent[index] = index;
      if (games[index].sources.empty() && !games[index].platform.empty()) games[index].sources = { games[index].platform };
    }
    const auto find = [&](auto &&self, int index) -> int {
      if (parent[index] != index) parent[index] = self(self, parent[index]);
      return parent[index];
    };
    const auto unite = [&](int left, int right) {
      left = find(find, left);
      right = find(find, right);
      if (left != right) parent[right] = left;
    };
    std::map<std::string, int> by_dir;
    std::map<std::string, int> by_exe;
    for (int index = 0; index < count; ++index) {
      if (!games[index].install_dir.empty()) {
        const std::string key = norm_key(games[index].install_dir);
        const auto it = by_dir.find(key);
        if (it == by_dir.end())
          by_dir.emplace(key, index);
        else
          unite(it->second, index);
      }
      if (!games[index].exe.empty()) {
        const std::string key = norm_key(games[index].exe);
        const auto it = by_exe.find(key);
        if (it == by_exe.end())
          by_exe.emplace(key, index);
        else
          unite(it->second, index);
      }
    }
    std::vector<Game> merged;
    std::vector<int> slot(count, -1);
    for (int index = 0; index < count; ++index) {
      const int root = find(find, index);
      if (slot[root] < 0) {
        slot[root] = static_cast<int>(merged.size());
        merged.push_back(games[index]);
      }
      else
        merge_game(merged[slot[root]], games[index]);
    }
    games.swap(merged);
  }
  void
  scan_libraries(const ScanRequest &request, const HostContext &context, ScanResult &result) {
    if (wanted(request, Source::xbox)) scan_xbox(context, result);
    if (wanted(request, Source::ea)) scan_ea(context, result);
    if (wanted(request, Source::ubisoft)) scan_ubisoft(context, result);
    if (wanted(request, Source::battlenet)) scan_battlenet(context, result);
    if (wanted(request, Source::riot)) scan_riot(context, result);
    if (wanted(request, Source::hoyoplay)) scan_hoyoplay(context, result);
    if (wanted(request, Source::wegame)) scan_wegame(context, result);
    if (wanted(request, Source::sparxie)) scan_sparxie(context, result);
  }
}  // namespace app_scan
