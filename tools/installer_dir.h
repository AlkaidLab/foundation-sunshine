#pragma once
// Where the program may be installed: the rules, as plain functions of a path and of what the machine says about it. Header only, std only: the installer shell
// (installer_dir_win.h gathers the facts on Windows) and the unit tests (a fake machine) both include it.
//
// The service runs the programs of the install folder as SYSTEM, so the folder must be one that an ordinary user cannot write to. Program Files is that by
// inheritance; any other folder is made so by the installer (installer_dir_win.h: protected DACL, same as Program Files). These rules only decide which
// folders are worth that: a fixed disk of this PC with a file system that has permissions (NTFS, ReFS), a folder that is new, empty or already ours, and not one
// that Windows, the user or a sync client owns.
//
// There are two kinds of answer. A *problem* is a folder that cannot work or would lose data (a network disk, a file system without permissions, the Windows folder, a synced
// folder, a folder with somebody's files in it, ...): never accepted. A *warning* is a folder that works and is protected itself, but sits somewhere that ordinary users or
// programs of the same user can rename or replace from above (a folder somebody made under the root of a data drive, the user's own folders): accepted after the person
// confirmed it (the page asks once, with a delay), by the silent switch /ALLOWUNSAFEPARENT, and never otherwise.
//
// The same function answers the page (a refused folder keeps the install buttons off and says why), the elevated worker (it never trusts the shell's answer) and
// the silent switch /DIR=.
//
// Which folder is the install folder: what the person typed is taken as it is. A folder picked in the system picker gets the product's folder name appended, as Inno Setup does
// after Browse (AppendDefaultDirName) and NSIS does with the last part of InstallDir; the picked folder itself is one click away (from_pick()).
// Authority: installer-inno-appenddefaultdirname-20261119, installer-nsis-installdir-20261119, installer-msi-pathedit-20261119, installer-wixui-installdir-20261119.
#include <cstdint>
#include <cwctype>
#include <optional>
#include <string>
#include <type_traits>
#include <string_view>
#include <vector>

namespace installer_dir {

  /** Why a folder is refused. The words are the codes of the setup UI (foundation-ui installerTexts.js `dir.err.<code>`). */
  enum class error {
    none,
    invalid,  // not a full path of a drive, or a part Windows does not allow
    chars,  // a character that the installer's scripts, PATH or the setup engine cannot carry
    not_fixed,  // not a fixed disk of this PC: network, removable, mapped (subst), optical
    fs,  // not NTFS or ReFS
    root,  // the root of a disk
    system,  // inside the Windows folder
    user,  // inside a user's folder: a warning, not a problem (the user and the programs that run as that user can replace the folder from above)
    synced,  // inside a synced folder (OneDrive and the like)
    parent,  // a folder above it can be renamed, replaced or re-permissioned by ordinary users: the program could be swapped out from above (a warning)
    not_empty,  // has files and is not an install of ours
    too_long,  // the longest path of the package would pass MAX_PATH
    no_space,  // the drive is too full
    no_access,  // cannot be opened or created
  };

  inline std::string_view
  code(const error e) {
    switch (e) {
      case error::none: return "";
      case error::invalid: return "invalid";
      case error::chars: return "chars";
      case error::not_fixed: return "not-fixed";
      case error::fs: return "fs";
      case error::root: return "root";
      case error::system: return "system";
      case error::user: return "user";
      case error::synced: return "synced";
      case error::parent: return "parent";
      case error::not_empty: return "not-empty";
      case error::too_long: return "too-long";
      case error::no_space: return "no-space";
      case error::no_access: return "no-access";
    }
    return "invalid";
  }

  /** The two codes that are warnings (the folder can be used after the person confirmed it); every other code is a problem. */
  inline bool
  is_warning(const error e) {
    return e == error::parent || e == error::user;
  }

  // The longest path inside an install, relative to the folder, with its leading backslash. The package itself stays well below it (scripts/ci/check-install-path-budget.py
  // measures the staged tree and fails when the package grows past this); the rest is what the host writes at run time: the cover and poster previews of
  // config\\covers are named after the app and a hash and reach 141 characters, so the budget holds a few more.
  constexpr size_t kLongestRelative = 150;
  constexpr size_t kPathMargin = 24;
  constexpr size_t kMaxPath = 260;  // MAX_PATH, with the terminating null: the setup engine, the batch scripts and the service wrapper work with it
  /** The longest folder path that is accepted, in UTF-16 units. */
  constexpr size_t kMaxLength = kMaxPath - 1 - kLongestRelative - kPathMargin;
  /** The space the install needs on its drive: the program (about 700 MB unpacked, the settings and covers of a long-used host come on top) and room for the first run. */
  constexpr std::uint64_t kNeededBytes = 1024ull * 1024 * 1024;

  /** The default folder name below Program Files. */
  inline constexpr std::wstring_view kDefaultName = L"Sunshine";
  /** The folder name that is appended to a folder the person picked: the product's name, as it stands in the Start menu. */
  inline constexpr std::wstring_view kProductFolder = L"Foundation Sunshine";
  /** How long the confirmation of a warning stays closed (the same seconds as the other risky questions of the product: foundation-ui RESET_SECONDS). */
  constexpr int kConfirmSeconds = 5;

  inline wchar_t
  fold(const wchar_t c) {
    return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
  }

  inline bool
  same_text(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
      if (fold(a[i]) != fold(b[i])) return false;
    return true;
  }

  inline bool
  starts_with_text(std::wstring_view s, std::wstring_view prefix) {
    return s.size() >= prefix.size() && same_text(s.substr(0, prefix.size()), prefix);
  }

  /** A typed or pasted path without what a person did not mean: spaces and tabs at both ends (a folder name never ends in a space), and one pair of quotes around it ("Copy as path" in Explorer gives `"D:\\x"`). */
  inline std::wstring
  tidy(std::wstring_view text) {
    const auto blank = [](const wchar_t c) { return c == L' ' || c == L'\t'; };
    while (!text.empty() && blank(text.front())) text.remove_prefix(1);
    while (!text.empty() && blank(text.back())) text.remove_suffix(1);
    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"') {
      text.remove_prefix(1);
      text.remove_suffix(1);
      while (!text.empty() && blank(text.front())) text.remove_prefix(1);
      while (!text.empty() && blank(text.back())) text.remove_suffix(1);
    }
    return std::wstring(text);
  }

  /** The path as the installer compares and stores it: backslashes, no trailing backslash except on a drive root ("D:\"). Nothing else is changed. */
  inline std::wstring
  normalise(std::wstring path) {
    for (auto &c : path)
      if (c == L'/') c = L'\\';
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
  }

  /** `path` is `root` or lies inside it (a whole path part, case-insensitive). Both are normalised. */
  inline bool
  inside(std::wstring_view path, std::wstring_view root) {
    if (root.empty() || path.size() < root.size() || !same_text(path.substr(0, root.size()), root)) return false;
    if (path.size() == root.size()) return true;
    return root.back() == L'\\' || path[root.size()] == L'\\';
  }

  /** The characters the batch scripts (delayed expansion, `&`, `^`, `%`), PATH (`;`), the setup engine's constants (`{`) and quoting cannot carry. */
  inline bool
  carries(const wchar_t c) {
    switch (c) {
      case L'!': case L'%': case L'^': case L'&': case L';': case L',': case L'=': case L'\'': case L'`': case L'{': case L'}': case L'$':
        return false;
      default:
        return true;
    }
  }

  inline bool
  reserved_name(std::wstring_view part) {
    const size_t dot = part.find(L'.');
    const std::wstring_view stem = dot == std::wstring_view::npos ? part : part.substr(0, dot);
    for (const std::wstring_view word : { L"con", L"prn", L"aux", L"nul", L"conin$", L"conout$" })
      if (same_text(stem, word)) return true;
    // COM1..COM9 and LPT1..LPT9, and with the superscript digits Windows also takes as digits there (U+00B9, U+00B2, U+00B3)
    if (stem.size() == 4 && (same_text(stem.substr(0, 3), L"com") || same_text(stem.substr(0, 3), L"lpt")) &&
        ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == 0x00B9 || stem[3] == 0x00B2 || stem[3] == 0x00B3))
      return true;
    return false;
  }

  /** The length in UTF-16 units, as Windows counts a path: where wchar_t is 32 bits (the tests on Linux) a character above U+FFFF is two units. */
  inline size_t
  utf16_length(std::wstring_view text) {
    size_t n = 0;
    for (const wchar_t c : text) {
      ++n;
      if ((static_cast<unsigned long long>(static_cast<std::make_unsigned_t<wchar_t>>(c)) >> 16) != 0) ++n;  // a unit above U+FFFF: a surrogate pair in UTF-16
    }
    return n;
  }

  struct shape {
    error problem = error::none;
    std::wstring path;  // normalised
    wchar_t drive = 0;  // upper case letter
    bool unc = false;
    bool root = false;
  };

  /**
   * What the text of the path alone says: a full path of a drive letter, parts Windows accepts, characters the scripts carry, not the root, not too long.
   * A UNC path is reported as `not_fixed` (it is a network place), `\\?\` and `\\.\` and `expand:` forms as `invalid`.
   */
  inline shape
  look(const std::wstring_view raw, const size_t max_length = kMaxLength) {
    shape out;
    out.path = normalise(std::wstring(raw));
    const std::wstring &p = out.path;
    if (p.empty()) {
      out.problem = error::invalid;
      return out;
    }
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') {
      out.unc = true;
      out.problem = (p.size() >= 3 && (p[2] == L'?' || p[2] == L'.')) ? error::invalid : error::not_fixed;
      return out;
    }
    if (p.size() < 3 || !((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) || p[1] != L':' || p[2] != L'\\') {
      // a bare "D:" is a drive-relative path, "expand:..." a setup-engine form: neither is a place
      out.problem = error::invalid;
      return out;
    }
    out.drive = static_cast<wchar_t>(std::towupper(static_cast<wint_t>(p[0])));
    for (size_t i = 0; i < p.size(); ++i) {
      const wchar_t c = p[i];
      if (c < 32 || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c == L'?' || c == L'*' || (c == L':' && i != 1)) {
        out.problem = error::invalid;
        return out;
      }
    }
    for (const wchar_t c : p)
      if (!carries(c)) {
        out.problem = error::chars;
        return out;
      }
    if (p.size() == 3) {
      out.root = true;
      out.problem = error::root;
      return out;
    }
    size_t begin = 3;
    while (begin <= p.size()) {
      size_t end = p.find(L'\\', begin);
      if (end == std::wstring::npos) end = p.size();
      const std::wstring_view part = std::wstring_view(p).substr(begin, end - begin);
      if (part.empty() || part == L"." || part == L".." || part.back() == L' ' || part.back() == L'.' || part.front() == L' ' || reserved_name(part)) {
        out.problem = error::invalid;
        return out;
      }
      begin = end + 1;
    }
    if (utf16_length(p) > max_length) out.problem = error::too_long;
    return out;
  }

  /** What the machine says about a folder (installer_dir_win.h fills it; the tests fake it). */
  struct facts {
    bool fixed_disk = false;  // DRIVE_FIXED, and not a mapped (subst) drive
    std::wstring file_system;  // "NTFS", "ReFS", "FAT32", "exFAT", ...
    bool exists = false;
    bool is_directory = false;  // when it exists
    bool reparse_point = false;  // a junction or a symbolic link on the way or at the folder itself, whether or not the folder exists yet
    bool empty = false;  // when it exists as a directory
    bool ours = false;  // an existing Sunshine install folder: the registered one, or one with the program and its uninstaller
    bool accessible = true;  // when it exists: could be listed
    std::optional<std::uint64_t> free_bytes;
    std::vector<std::wstring> system_roots;  // folders that may not hold the install: the Windows folder, the recycle bin and the like
    std::vector<std::wstring> user_roots;  // C:\Users, the profile, Desktop, Documents, Downloads (including a redirected one)
    std::vector<std::wstring> synced_roots;  // OneDrive's folders, as the client reports them
    std::vector<std::wstring> synced_names;  // a path part that names a sync client's folder (OneDrive, Dropbox, ...)
    // An ordinary user (or a program running as one) can rename, delete or re-permission a folder above the install folder, or delete the folder out of its parent: then the
    // program that the service starts as SYSTEM can be swapped for another one from above, whatever the protection of the install folder itself. Program Files is protected on
    // every level; below the root of another drive only folders that this setup creates (protected) or that already are (owned by the system, no write for users) are safe.
    bool parent_unsafe = false;
  };

  inline bool
  file_system_ok(std::wstring_view name) {
    return same_text(name, L"NTFS") || same_text(name, L"ReFS");
  }

  /** The names of the folders sync clients create, matched against every part of the path (a part that is the name, or starts with it followed by " " or "-"). */
  inline const std::vector<std::wstring> &
  default_synced_names() {
    static const std::vector<std::wstring> names = { L"OneDrive", L"Dropbox", L"Google Drive", L"GoogleDrive", L"iCloudDrive", L"iCloud Drive", L"Box Sync", L"pCloudDrive", L"MEGA", L"Nextcloud", L"Seafile", L"Sync.com" };
    return names;
  }

  inline bool
  names_a_synced_folder(std::wstring_view path, const std::vector<std::wstring> &names) {
    size_t begin = 3;
    while (begin < path.size()) {
      size_t end = path.find(L'\\', begin);
      if (end == std::wstring_view::npos) end = path.size();
      const std::wstring_view part = path.substr(begin, end - begin);
      for (const auto &name : names) {
        if (!starts_with_text(part, name)) continue;
        if (part.size() == name.size() || part[name.size()] == L' ' || part[name.size()] == L'-') return true;
      }
      begin = end + 1;
    }
    return false;
  }

  struct verdict {
    error problem = error::none;  // the first thing that makes the folder unusable
    error warning = error::none;  // parent or user: usable once the person confirmed it; set only when there is no problem
    std::wstring path;  // normalised
  };

  /**
   * The whole decision, in the order a person can act on it: the text of the path, then the disk (fixed, a file system with permissions, room), then the place
   * (not Windows, not a synced folder; a user folder or an unsafe parent is a warning), then the folder itself (opens, empty or ours).
   * `needed` is the space the install needs; `max_length` the longest path; both are parameters so that the tests can move them.
   */
  inline verdict
  check(const std::wstring_view raw, const facts &machine, const std::uint64_t needed = kNeededBytes, const size_t max_length = kMaxLength) {
    verdict out;
    const shape s = look(raw, max_length);
    out.path = s.path;
    if (s.problem != error::none && s.problem != error::too_long) {
      out.problem = s.problem;
      return out;
    }
    if (!machine.fixed_disk) {
      out.problem = error::not_fixed;
      return out;
    }
    if (!file_system_ok(machine.file_system)) {
      out.problem = error::fs;
      return out;
    }
    if (s.problem == error::too_long) {
      out.problem = error::too_long;
      return out;
    }
    for (const auto &root : machine.system_roots)
      if (inside(s.path, root)) {
        out.problem = error::system;
        return out;
      }
    // a synced folder is named before the user folder it usually sits in (OneDrive lives in the profile)
    for (const auto &root : machine.synced_roots)
      if (inside(s.path, root)) {
        out.problem = error::synced;
        return out;
      }
    if (names_a_synced_folder(s.path, machine.synced_names)) {
      out.problem = error::synced;
      return out;
    }
    // the user's folders and a parent that others can replace are warnings: they do not stop the folder from working, and the rest is looked at too
    for (const auto &root : machine.user_roots)
      if (inside(s.path, root)) {
        out.warning = error::user;
        break;
      }
    if (machine.parent_unsafe && out.warning == error::none) out.warning = error::parent;
    // a refused folder shows its reason only, never a warning with it
    const auto refuse = [&out](const error e) {
      out.problem = e;
      out.warning = error::none;
      return out;
    };
    // a junction or symbolic link on the way, whether or not the folder itself exists yet: the program would land somewhere else
    if (machine.reparse_point) return refuse(error::no_access);
    if (machine.exists) {
      // a file in the way, or a folder that cannot be listed
      if (!machine.is_directory || !machine.accessible) return refuse(error::no_access);
      if (!machine.empty && !machine.ours) return refuse(error::not_empty);
    }
    // an install that is already there needs no new room for what it holds, but the update still unpacks: the same rule for both
    if (machine.free_bytes && *machine.free_bytes < needed) return refuse(error::no_space);
    return out;
  }

  // ---------------------------------------------------------------- which folder: a picked one, a typed one, a safe one

  struct pick {
    std::wstring path;  // the install folder
    bool appended = false;  // the product's folder name was added to what was picked (the picked folder itself is one click away)
    bool redirected = false;  // the picked folder had somebody's files in it, which is why a folder inside it is used
  };

  /**
   * The install folder for a folder picked in the system picker. The product's folder name is appended (Inno Setup after Browse, NSIS with the last part of InstallDir) unless the
   * picked folder already is the default folder, is named like the product, or is an install of ours (`ours`: it is updated in place). The root of a disk gets it too: "D:\" is
   * never an install folder. `occupied`: the picked folder exists, is not an install of ours and has something in it (the root of a disk does not count): the person is told that
   * the files are left alone and a folder inside the picked one is used. Typed text is not passed here: it is the folder as typed.
   */
  inline pick
  from_pick(std::wstring_view picked, const bool ours, const bool occupied, std::wstring_view default_dir) {
    pick out;
    out.path = normalise(std::wstring(picked));
    if (out.path.empty()) return out;
    if (ours || same_text(out.path, default_dir)) return out;
    const size_t slash = out.path.find_last_of(L'\\');
    if (slash != std::wstring::npos && slash + 1 < out.path.size() && same_text(std::wstring_view(out.path).substr(slash + 1), kProductFolder)) return out;
    if (out.path.back() != L'\\') out.path.push_back(L'\\');
    out.path.append(kProductFolder);
    out.appended = true;
    out.redirected = occupied;
    return out;
  }

  /**
   * The safe place to offer for a folder that only has a warning: the default folder when it is on the system drive (the drive of `default_dir`), else the product folder directly
   * on the drive of the path ("D:\Foundation Sunshine"). Directly on the root there is no folder above it that others could replace, unless the root itself lets ordinary users delete folders; the caller therefore checks the candidate with the same rules and offers it only when it has no warning of its own. The folder itself is protected by the setup.
   */
  inline std::wstring
  safe_alternative(std::wstring_view path, std::wstring_view default_dir) {
    if (path.size() < 2 || path[1] != L':' || default_dir.empty()) return std::wstring(default_dir);
    const wchar_t drive = fold(path[0]);
    if (drive == fold(default_dir[0])) return std::wstring(default_dir);
    std::wstring out(1, static_cast<wchar_t>(std::towupper(static_cast<wint_t>(path[0]))));
    out += L":\\";
    out.append(kProductFolder);
    return out;
  }

  // ---------------------------------------------------------------- command lines

  /** A command line in its arguments, the Windows way for quotes: a quote opens and closes a group, nothing is escaped (a path ends in a backslash: `/DIR="D:\x\"`). */
  inline std::vector<std::wstring>
  split_arguments(std::wstring_view line) {
    std::vector<std::wstring> out;
    std::wstring current;
    bool quoted = false, started = false;
    for (const wchar_t c : line) {
      if (c == L'"') {
        quoted = !quoted;
        started = true;
      }
      else if ((c == L' ' || c == L'\t') && !quoted) {
        if (started) out.push_back(current);
        current.clear();
        started = false;
      }
      else {
        current.push_back(c);
        started = true;
      }
    }
    if (started) out.push_back(current);
    return out;
  }

  /** The value after `key` as its own argument: `--dir "D:\My Games\Sunshine"` gives `D:\My Games\Sunshine`; empty when absent. */
  inline std::wstring
  option_value(std::wstring_view line, std::wstring_view key) {
    const auto args = split_arguments(line);
    for (size_t i = 0; i + 1 < args.size(); ++i)
      if (args[i] == key) return args[i + 1];
    return {};
  }

  /** The `/DIR=` switch of the setup engine, in either spelling: `/DIR="D:\a b"`, `"/DIR=D:\a b"`, `/DIR=D:\ab`. nullopt when it is not there. */
  inline std::optional<std::wstring>
  dir_switch(std::wstring_view line) {
    for (const auto &arg : split_arguments(line))
      if (arg.size() >= 5 && starts_with_text(arg, L"/DIR=")) return arg.substr(5);
    return std::nullopt;
  }

  /** A whole-argument switch of the silent command line (`/ALLOWUNSAFEPARENT`), any case. */
  inline bool
  has_switch(std::wstring_view line, std::wstring_view name) {
    for (const auto &arg : split_arguments(line))
      if (same_text(arg, name)) return true;
    return false;
  }

  /** `/DIR=...` for the engine's command line. The path was checked: it holds no quote, so the quoted form cannot be left early. */
  inline std::wstring
  engine_switch(std::wstring_view path) {
    return L" /DIR=\"" + std::wstring(path) + L"\"";
  }

}  // namespace installer_dir
