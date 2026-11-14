#include "src/sparxie/zzz_file.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <system_error>

namespace sparxie {
  namespace {
    std::vector<std::uint8_t>
    read_file_bytes(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      if (!in) return {};
      return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    // A document parsed into the spans of its original bytes. Nothing is
    // re-serialized: an edit replaces the span of one value, so the key order,
    // padding, number formats and escapes of every other byte of the game's
    // document survive a write.
    struct Node {
      enum class Kind { object,
        array,
        scalar };
      Kind kind = Kind::scalar;
      std::size_t begin = 0;
      std::size_t end = 0;
      struct Member {
        std::string key;
        std::size_t key_begin = 0;
        std::unique_ptr<Node> value;
      };
      std::vector<Member> members;
      std::vector<std::unique_ptr<Node>> items;
    };

    std::string
    decode_json_string(std::string_view raw) {
      std::string out;
      if (raw.size() < 2 || raw.front() != '"') return out;
      for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
        const char c = raw[i];
        if (c != '\\' || i + 1 >= raw.size() - 1) {
          out.push_back(c);
          continue;
        }
        ++i;
        switch (raw[i]) {
          case 'n':
            out.push_back('\n');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'u': {
            if (i + 4 >= raw.size() - 1) return out;
            unsigned code = 0;
            for (int h = 0; h < 4; ++h) {
              const char digit = raw[++i];
              code <<= 4;
              if (digit >= '0' && digit <= '9')
                code |= unsigned(digit - '0');
              else if (digit >= 'a' && digit <= 'f')
                code |= unsigned(digit - 'a' + 10);
              else if (digit >= 'A' && digit <= 'F')
                code |= unsigned(digit - 'A' + 10);
              else
                return out;
            }
            if (code < 0x80)
              out.push_back(char(code));
            else if (code < 0x800) {
              out.push_back(char(0xC0 | (code >> 6)));
              out.push_back(char(0x80 | (code & 0x3F)));
            }
            else {
              out.push_back(char(0xE0 | (code >> 12)));
              out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
              out.push_back(char(0x80 | (code & 0x3F)));
            }
            break;
          }
          default:
            out.push_back(raw[i]);
            break;
        }
      }
      return out;
    }

    struct Parser {
      std::string_view text;
      std::size_t at = 0;
      bool ok = true;

      void
      ws() {
        while (at < text.size()) {
          const char c = text[at];
          if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            ++at;
          else
            break;
        }
      }

      bool
      literal(std::string_view lit) {
        if (text.size() - at < lit.size() || text.substr(at, lit.size()) != lit) return false;
        at += lit.size();
        return true;
      }

      bool
      scan_string() {
        if (at >= text.size() || text[at] != '"') return false;
        ++at;
        while (at < text.size()) {
          const char c = text[at++];
          if (c == '\\') {
            ++at;
            continue;
          }
          if (c == '"') return true;
        }
        return false;
      }

      std::unique_ptr<Node>
      parse_value() {
        ws();
        if (at >= text.size()) {
          ok = false;
          return nullptr;
        }
        const std::size_t begin = at;
        if (text[at] == '{') return parse_object(begin);
        if (text[at] == '[') return parse_array(begin);
        if (text[at] == '"') {
          if (!scan_string()) {
            ok = false;
            return nullptr;
          }
          return node(Node::Kind::scalar, begin, at);
        }
        if (literal("true") || literal("false") || literal("null")) return node(Node::Kind::scalar, begin, at);
        std::size_t digits = 0;
        while (at < text.size()) {
          const char c = text[at];
          const bool number = (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
          if (!number) break;
          ++at;
          ++digits;
        }
        if (digits == 0) {
          ok = false;
          return nullptr;
        }
        return node(Node::Kind::scalar, begin, at);
      }

      std::unique_ptr<Node>
      parse_object(std::size_t begin) {
        ++at;  // the opening brace
        auto out = std::make_unique<Node>();
        out->kind = Node::Kind::object;
        out->begin = begin;
        ws();
        if (at < text.size() && text[at] == '}') {
          out->end = ++at;
          return out;
        }
        while (ok) {
          ws();
          const std::size_t key_begin = at;
          if (!scan_string()) {
            ok = false;
            break;
          }
          const std::size_t key_end = at;
          ws();
          if (at >= text.size() || text[at] != ':') {
            ok = false;
            break;
          }
          ++at;
          auto value = parse_value();
          if (!ok || !value) break;
          out->members.push_back({ decode_json_string(text.substr(key_begin, key_end - key_begin)), key_begin, std::move(value) });
          ws();
          if (at < text.size() && text[at] == ',') {
            ++at;
            continue;
          }
          if (at < text.size() && text[at] == '}') {
            out->end = ++at;
            return out;
          }
          ok = false;
        }
        return nullptr;
      }

      std::unique_ptr<Node>
      parse_array(std::size_t begin) {
        ++at;  // the opening bracket
        auto out = std::make_unique<Node>();
        out->kind = Node::Kind::array;
        out->begin = begin;
        ws();
        if (at < text.size() && text[at] == ']') {
          out->end = ++at;
          return out;
        }
        while (ok) {
          auto value = parse_value();
          if (!ok || !value) break;
          out->items.push_back(std::move(value));
          ws();
          if (at < text.size() && text[at] == ',') {
            ++at;
            continue;
          }
          if (at < text.size() && text[at] == ']') {
            out->end = ++at;
            return out;
          }
          ok = false;
        }
        return nullptr;
      }

      std::unique_ptr<Node>
      node(Node::Kind kind, std::size_t begin, std::size_t end) {
        auto out = std::make_unique<Node>();
        out->kind = kind;
        out->begin = begin;
        out->end = end;
        return out;
      }
    };

    const Node *
    member(const Node &object, std::string_view key) {
      if (object.kind != Node::Kind::object) return nullptr;
      for (const auto &entry : object.members)
        if (entry.key == key) return entry.value.get();
      return nullptr;
    }

    int
    int_of(const Node &node, std::string_view text, int fallback) {
      if (node.kind != Node::Kind::scalar) return fallback;
      try {
        const json value = json::parse(std::string(text.substr(node.begin, node.end - node.begin)));
        if (value.is_number_integer()) return value.get<int>();
      }
      catch (const std::exception &) {
      }
      return fallback;
    }

    struct Edit {
      std::size_t begin;
      std::size_t end;
      std::string text;
    };

    std::string
    apply_edits(std::string_view source, std::vector<Edit> edits) {
      std::sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) { return a.begin > b.begin; });
      std::string out(source);
      for (const Edit &edit : edits) out.replace(edit.begin, edit.end - edit.begin, edit.text);
      return out;
    }

    // A new member takes the indentation of the object's last member, so its
    // line sits at the level of its neighbours.
    void
    set_member(std::vector<Edit> &edits, const Node &object, std::string_view source, const char *key, const std::string &value) {
      const Node *found = member(object, key);
      if (found) {
        edits.push_back({ found->begin, found->end, value });
        return;
      }
      std::string lead = ",";
      std::string indent;
      if (!object.members.empty()) {
        const std::size_t key_begin = object.members.back().key_begin;
        const std::size_t line = source.find_last_of('\n', key_begin);
        const std::size_t from = line == std::string_view::npos ? 0 : line + 1;
        indent = std::string(source.substr(from, key_begin - from));
        lead = ",\n";
      }
      const std::size_t at = object.members.empty() ? object.begin + 1 : object.end - 1;
      edits.push_back({ at, at, lead + indent + "\"" + std::string(key) + "\" : " + value });
    }

    // The stored numbers follow the game's own option order: frame rate 30,
    // 60, unlimited; render scale 0.8, 1.0, 1.2; preset high, medium, low,
    // custom. The read fallbacks are the game's defaults for an entry the
    // person has never changed in-game.
    int
    fps_to_store(int fps) { return fps == 30 ? 0 : fps == 0 ? 2 :
                                                              1; }
    int
    fps_from_store(int data) { return data == 0 ? 30 : data == 2 ? 0 :
                                                                   60; }
    int
    scale_to_store(double scale) { return scale < 0.9 ? 0 : scale > 1.1 ? 2 :
                                                                          1; }
    double
    scale_from_store(int data) { return data == 0 ? 0.8 : data == 2 ? 1.2 :
                                                                      1.0; }
    int
    preset_to_store(const std::string &preset) {
      if (preset == "high") return 0;
      if (preset == "medium") return 1;
      if (preset == "low" || preset == "lowest") return 2;
      if (preset == "custom") return 3;
      return 1;
    }
    std::string
    preset_from_store(int data) { return data == 0 ? "high" : data == 1 ? "medium" :
                                                            data == 2   ? "low" :
                                                                          "custom"; }

    std::string
    system_setting_node(int data) {
      return "{\"$Type\":\"MoleMole.SystemSettingLocalData\",\"Version\":0,\"Data\":" + std::to_string(data) + "}";
    }

    struct Decoded {
      std::string text;
      std::unique_ptr<Node> root;
      const std::vector<std::uint8_t> *magic = nullptr;
    };

    bool
    decode(const std::vector<std::uint8_t> &bytes, const std::vector<std::vector<std::uint8_t>> &magics, Decoded &out) {
      for (const auto &magic : magics) {
        if (magic.empty()) continue;
        const std::string text = sleepy_read(bytes, magic);
        if (text.empty()) continue;
        Parser parser { text };
        std::unique_ptr<Node> root = parser.parse_value();
        parser.ws();
        if (!parser.ok || !root || parser.at != text.size() || root->kind != Node::Kind::object) continue;
        out.text = text;
        out.root = std::move(root);
        out.magic = &magic;
        return true;
      }
      return false;
    }

    void
    extract(const Node &root, std::string_view text, GameValues &values) {
      const Node *map = member(root, "SystemSettingDataMap");
      auto data = [&](const char *key, int fallback) {
        const Node *entry = map ? member(*map, key) : nullptr;
        const Node *node = entry ? member(*entry, "Data") : nullptr;
        return node ? int_of(*node, text, fallback) : fallback;
      };
      values.fps = fps_from_store(data("110", 1));
      values.vsync = data("8", 1) != 0;
      values.render_scale = scale_from_store(data("9", 1));
      values.preset = preset_from_store(data("3", 1));
      const Node *hdr = member(root, "HDRSettingRecordState");
      values.hdr = hdr ? int_of(*hdr, text, 0) != 0 : false;
    }
  }  // namespace

  std::vector<std::vector<std::uint8_t>>
  load_zzz_magics(const fs::path &host_data, const std::vector<fs::path> &asset_roots) {
    std::vector<std::vector<std::uint8_t>> out;
    // One explicit key in the host data directory replaces the shipped set.
    std::vector<std::uint8_t> override_key = read_file_bytes(host_data / "zzz-magic.bin");
    if (!override_key.empty()) {
      out.push_back(std::move(override_key));
      return out;
    }
    for (const fs::path &root : asset_roots) {
      // The shipped key covers every region; the per-region names are read
      // as well, so a future split ships as files without a code change.
      for (const char *name : { "zzz-magic.bin", "zzz-magic.global.bin", "zzz-magic.cn.bin", "zzz-magic.bili.bin" }) {
        std::vector<std::uint8_t> key = read_file_bytes(root / "sparxie" / name);
        if (key.empty()) continue;
        if (std::find(out.begin(), out.end(), key) != out.end()) continue;
        out.push_back(std::move(key));
      }
    }
    return out;
  }

  bool
  zzz_read_general_data(const fs::path &file, const std::vector<std::vector<std::uint8_t>> &magics, GameValues &values) {
    const std::vector<std::uint8_t> bytes = read_file_bytes(file);
    if (bytes.empty()) return false;
    Decoded decoded;
    if (!decode(bytes, magics, decoded)) return false;
    extract(*decoded.root, decoded.text, values);
    zzz_fill_extra(decoded.text, values);
    return true;
  }

  bool
  zzz_write_general_data(const fs::path &file, const std::vector<std::vector<std::uint8_t>> &magics, const GameValues &values, const std::set<std::string> &fields, std::string &error) {
    auto want = [&](const char *key) { return fields.empty() || fields.count(key) > 0; };
    const std::vector<std::uint8_t> bytes = read_file_bytes(file);
    if (bytes.empty()) {
      error = "the game has not written its settings file yet";
      return false;
    }
    Decoded decoded;
    if (!decode(bytes, magics, decoded)) {
      error = "the settings file does not decode with the shipped key (a newer game version?)";
      return false;
    }
    const Node *map = member(*decoded.root, "SystemSettingDataMap");
    if (map && map->kind != Node::Kind::object) map = nullptr;
    std::vector<Edit> edits;
    auto put = [&](const char *key, int data) {
      const Node *entry = map ? member(*map, key) : nullptr;
      if (entry && entry->kind == Node::Kind::object) {
        const Node *node = member(*entry, "Data");
        if (node && node->kind == Node::Kind::scalar) {
          edits.push_back({ node->begin, node->end, std::to_string(data) });
          return;
        }
        set_member(edits, *entry, decoded.text, "Data", std::to_string(data));
        return;
      }
      if (entry) {  // a broken entry is replaced, never duplicated
        edits.push_back({ entry->begin, entry->end, system_setting_node(data) });
        return;
      }
      set_member(edits, *map, decoded.text, key, system_setting_node(data));
    };
    const bool wants_map = want("fps") || want("vsync") || want("renderScale") || want("preset");
    if (map) {
      if (want("fps")) put("110", fps_to_store(values.fps));
      if (want("vsync")) put("8", values.vsync ? 1 : 0);
      if (want("renderScale")) put("9", scale_to_store(values.render_scale));
      if (want("preset")) put("3", preset_to_store(values.preset));
    }
    else if (wants_map) {
      std::string body;
      auto add = [&](const char *key, int data) {
        if (!body.empty()) body += ",";
        body += "\"" + std::string(key) + "\":" + system_setting_node(data);
      };
      if (want("fps")) add("110", fps_to_store(values.fps));
      if (want("vsync")) add("8", values.vsync ? 1 : 0);
      if (want("renderScale")) add("9", scale_to_store(values.render_scale));
      if (want("preset")) add("3", preset_to_store(values.preset));
      set_member(edits, *decoded.root, decoded.text, "SystemSettingDataMap", "{" + body + "}");
    }
    if (want("hdr")) set_member(edits, *decoded.root, decoded.text, "HDRSettingRecordState", values.hdr ? "1" : "0");
    const std::vector<ZzzEdit> extra = zzz_extra_edits(values, fields);
    bool extra_map = false;
    for (const ZzzEdit &edit : extra)
      if (!edit.root) extra_map = true;
    if (map) {
      for (const ZzzEdit &edit : extra)
        if (!edit.root) put(edit.slot.c_str(), edit.data);
    }
    else if (extra_map && !wants_map) {
      std::string body;
      for (const ZzzEdit &edit : extra) {
        if (edit.root) continue;
        if (!body.empty()) body += ",";
        body += "\"" + edit.slot + "\":" + system_setting_node(edit.data);
      }
      set_member(edits, *decoded.root, decoded.text, "SystemSettingDataMap", "{" + body + "}");
    }
    for (const ZzzEdit &edit : extra)
      if (edit.root) set_member(edits, *decoded.root, decoded.text, edit.slot.c_str(), std::to_string(edit.data));

    const std::string next = apply_edits(decoded.text, edits);
    const auto encoded = sleepy_write(next, *decoded.magic);
    if (sleepy_read(encoded, *decoded.magic) != next) {
      error = "the container did not round-trip the edited document";
      return false;
    }
    const fs::path temp = file.parent_path() / (file.filename().string() + ".sparxie-tmp");
    {
      std::ofstream out(temp, std::ios::binary | std::ios::trunc);
      if (!out) {
        error = "the temporary settings file could not be written";
        return false;
      }
      out.write(reinterpret_cast<const char *>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
      out.flush();
      if (!out) {
        error = "the temporary settings file could not be written";
        out.close();
        std::error_code remove_ec;
        fs::remove(temp, remove_ec);
        return false;
      }
    }
    if (read_file_bytes(temp) != encoded) {
      std::error_code remove_ec;
      fs::remove(temp, remove_ec);
      error = "the temporary settings file did not read back identically";
      return false;
    }
    std::error_code ec;
    fs::rename(temp, file, ec);
    if (ec) {
      std::error_code remove_ec;
      fs::remove(temp, remove_ec);
      error = "the settings file could not be replaced: " + ec.message();
      return false;
    }
    return true;
  }
}  // namespace sparxie
