#pragma once
#include "src/sparxie/model.h"
#include "src/sparxie/settings.h"
#include <set>
namespace sparxie {
  // The key bytes that decode the GENERAL_DATA.bin container of Zenless Zone
  // Zero. The shipped file under assets/sparxie supplies them; a file named
  // zzz-magic.bin in the host data directory replaces the whole set.
  std::vector<std::vector<std::uint8_t>>
  load_zzz_magics(const fs::path &host_data, const std::vector<fs::path> &asset_roots);

  // Reads the file-backed settings (fps, vsync, renderScale, preset, hdr) of
  // one install. Returns false when no key decodes the file; the values keep
  // what the caller put in, so the caller reports the fields unavailable.
  bool
  zzz_read_general_data(const fs::path &file, const std::vector<std::vector<std::uint8_t>> &magics, GameValues &values);

  // Writes only the chosen fields. The current file must decode with one of
  // the keys: the edit replaces the stored numbers while every other byte of
  // the document keeps its place, the new bytes go to a temporary file that
  // is read back and verified before it replaces the game's file, and a file
  // that does not decode is never touched.
  bool
  zzz_write_general_data(const fs::path &file, const std::vector<std::vector<std::uint8_t>> &magics, const GameValues &values, const std::set<std::string> &fields, std::string &error);

  // The file-backed items beyond fps, vsync, render scale, preset and HDR.
  // Tokens and defaults follow Collapse Zenless GeneralData (d478585).
  json
  zzz_file_catalog();

  struct ZzzEdit {
    bool root = false;
    std::string slot;
    int data = 0;
  };
  void
  zzz_fill_extra(const std::string &text, GameValues &values);
  std::vector<ZzzEdit>
  zzz_extra_edits(const GameValues &values, const std::set<std::string> &fields);
}  // namespace sparxie
