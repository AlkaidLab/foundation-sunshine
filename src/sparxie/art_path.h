#pragma once

#include "src/sparxie/model.h"

namespace sparxie {
  // Artwork belongs only to a known game and region, never to a filesystem path.
  inline bool
  valid_art_install(std::string_view id, std::string_view game, std::string_view region) {
    bool known_game = false;
    bool known_region = false;
    for (const auto value : kGames) known_game |= value == game;
    for (const auto value : kRegions) known_region |= value == region;
    if (!known_game || !known_region) return false;
    const std::string base = std::string(game) + "-" + std::string(region);
    if (id == base) return true;
    if (id.size() <= base.size() + 1 || id.size() > base.size() + 21 || id.substr(0, base.size()) != base || id[base.size()] != '-') return false;
    for (const char ch : id.substr(base.size() + 1)) {
      if (ch < '0' || ch > '9') return false;
    }
    return true;
  }

  inline bool
  valid_art_install(std::string_view id) {
    for (const auto game : kGames) {
      for (const auto region : kRegions) {
        if (valid_art_install(id, game, region)) return true;
      }
    }
    return false;
  }

  inline bool
  valid_art_version(std::string_view version) {
    if (version == "." || version == "..") return false;
    for (const unsigned char ch : version) {
      if (ch < 32 || ch == 127 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') return false;
    }
    return true;
  }

  // Resolve symlinks/junctions as well as lexical traversal before serving a file.
  inline bool
  art_file_within(const fs::path &root, const fs::path &file, fs::path *resolved = nullptr) {
    std::error_code ec;
    const auto canonical_root = fs::canonical(root, ec);
    if (ec) return false;
    const auto canonical_file = fs::canonical(file, ec);
    if (ec) return false;
    auto parent = canonical_root.begin();
    auto child = canonical_file.begin();
    for (; parent != canonical_root.end(); ++parent, ++child) {
      if (child == canonical_file.end() || *parent != *child) return false;
    }
    if (child == canonical_file.end()) return false;
    if (resolved) *resolved = canonical_file;
    return true;
  }
}  // namespace sparxie
