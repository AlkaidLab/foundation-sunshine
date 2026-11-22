#pragma once
// The Windows side of installer_dir.h: what this PC says about a folder (drive type, file system, room, known folders), the system folder picker, and the folder
// that the elevated worker creates for the install, with the same protection as Program Files.
//
// Why the folder is protected: the service runs the program of the install folder as SYSTEM. A folder below the root of another drive inherits "Authenticated Users:
// Modify" from that root, so any local user could replace sunshine.exe or sunshinesvc.exe and be SYSTEM at the next start. The worker therefore creates every folder it
// makes with a protected DACL, and the setup engine asserts the same DACL (cmake/packaging/sunshine.iss.in, HardenInstallDir). The DACL, in SDDL:
//   D:P  protected: nothing is inherited from the parent
//   (A;OICI;FA;;;SY)   SYSTEM: full control, this folder and everything below
//   (A;OICI;FA;;;BA)   Administrators: full control
//   (A;OICI;FA;;;S-1-5-80-...)  NT SERVICE\TrustedInstaller: full control
//   (A;OICI;0x1200a9;;;BU)  Users: read and execute (FILE_GENERIC_READ | FILE_GENERIC_EXECUTE)
//   (A;OICI;0x1200a9;;;AC)  ALL APPLICATION PACKAGES: read and execute, as Program Files has it
// Authority: installer-ms-sddl-ace-strings-20261119 (ACE strings, the AC and BA and SY and BU aliases, the OI and CI flags), installer-ms-well-known-sids-20261119
// (the TrustedInstaller SID), installer-ms-createdirectoryw-20261119 (lpSecurityDescriptor sets the DACL the directory is created with),
// installer-ms-setnamedsecurityinfow-20261119 (OWNER_SECURITY_INFORMATION, DACL_SECURITY_INFORMATION, PROTECTED_DACL_SECURITY_INFORMATION and the privileges
// needed), installer-ms-getdrivetypew-20261119, installer-ms-getvolumeinformationw-20261119, installer-ms-querydosdevicew-20261119 (a subst drive is `\??\`),
// installer-ms-shgetknownfolderpath-20261119, installer-ms-ifileopendialog-20261119 (FOS_PICKFOLDERS).
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>
#include <knownfolders.h>
#include <sddl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <string>
#include <vector>

#include "installer_dir.h"

namespace installer_dir_win {

  constexpr wchar_t kProtectedSddl[] =
    L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464)(A;OICI;0x1200a9;;;BU)(A;OICI;0x1200a9;;;AC)";

  inline std::wstring
  env_value(const wchar_t *name) {
    wchar_t buffer[2048] {};
    const DWORD n = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    return n == 0 || n >= std::size(buffer) ? std::wstring() : std::wstring(buffer, n);
  }

  /** The 64-bit Program Files of this PC (the setup engine's {autopf}): ProgramW6432 for a 32-bit process, ProgramFiles otherwise. */
  inline std::wstring
  program_files() {
    std::wstring dir = env_value(L"ProgramW6432");
    if (dir.empty()) dir = env_value(L"ProgramFiles");
    if (dir.empty()) dir = L"C:\\Program Files";
    return installer_dir::normalise(dir);
  }

  /** The folder the engine uses when it is given none and nothing is installed: Program Files\Sunshine. */
  inline std::wstring
  default_dir() {
    return program_files() + L"\\" + std::wstring(installer_dir::kDefaultName);
  }

  /** The path is the Program Files folder or lies in it: it inherits that folder's protection and needs none of its own. */
  inline bool
  under_program_files(const std::wstring &path) {
    return installer_dir::inside(installer_dir::normalise(path), program_files());
  }

  inline std::wstring
  known_folder(const KNOWNFOLDERID &id) {
    PWSTR raw = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &raw)) && raw) out = installer_dir::normalise(raw);
    if (raw) CoTaskMemFree(raw);
    return out;
  }

  inline std::wstring
  registered_install_dir() {
    wchar_t buffer[1024] {};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AlkaidLab\\Sunshine", L"InstallDir", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
    return installer_dir::normalise(buffer);
  }

  inline bool
  file_exists(const std::wstring &path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
  }

  /** The existing folder that `path` is in or is: itself, or the nearest parent that exists (for the room on its drive). */
  inline std::wstring
  nearest_existing(std::wstring path) {
    while (path.size() > 3 && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      const size_t slash = path.find_last_of(L'\\');
      if (slash == std::wstring::npos || slash < 3) {
        path = path.substr(0, 3);
        break;
      }
      path.resize(slash);
    }
    return path;
  }

  /**
   * The path with its existing part in long names. A folder has a long name and an 8.3 short name, and "D:\\DOCUME~1" is "D:\\Documents": the rules compare names, so both
   * sides have to be in the same form. The part that does not exist yet is left as it is (it is created by the worker with the name that the person chose).
   */
  inline std::wstring
  long_form(const std::wstring &path) {
    const std::wstring existing = nearest_existing(path);
    wchar_t buffer[2048] {};
    const DWORD n = GetLongPathNameW(existing.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) return path;
    return installer_dir::normalise(std::wstring(buffer, n)) + path.substr(existing.size());
  }

  /** A mapped (subst) drive answers QueryDosDevice with `\??\C:\folder`; a real volume with `\Device\HarddiskVolumeN`. */
  inline bool
  is_substituted(wchar_t drive) {
    const wchar_t name[3] = { drive, L':', 0 };
    wchar_t target[1024] {};
    if (QueryDosDeviceW(name, target, static_cast<DWORD>(std::size(target))) == 0) return false;
    return std::wstring_view(target).starts_with(L"\\??\\");
  }

  /** SYSTEM, Administrators and NT SERVICE\TrustedInstaller: the owners and the holders of rights that leave a folder safe. */
  inline bool
  is_trusted_sid(PSID sid) {
    for (const wchar_t *text : { L"S-1-5-18", L"S-1-5-32-544", L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464" }) {
      PSID known = nullptr;
      if (!ConvertStringSidToSidW(text, &known)) continue;
      const bool same = EqualSid(sid, known) != FALSE;
      LocalFree(known);
      if (same) return true;
    }
    return false;
  }

  /**
   * Whether somebody who is not trusted could rename, delete, re-permission or (with `children`) delete the contents of `dir`: an allow entry that applies to the folder itself
   * and carries DELETE, WRITE_DAC, WRITE_OWNER, FILE_DELETE_CHILD or GENERIC_ALL for anyone but SYSTEM, Administrators and TrustedInstaller; a folder without a DACL; an owner who
   * is not one of those (the owner may always change the permissions). An unreadable folder counts as unsafe. `owner` = false is for the root of a drive, whose owner and own
   * DELETE do not matter: only the right to delete what lies in it does.
   */
  inline bool
  others_can_replace(const std::wstring &dir, const bool owner) {
    constexpr DWORD kOnItself = DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_ALL;
    constexpr DWORD kInIt = FILE_DELETE_CHILD | GENERIC_ALL;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner_sid = nullptr;
    PACL dacl = nullptr;
    if (GetNamedSecurityInfoW(dir.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner_sid, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS)
      return true;
    bool unsafe = dacl == nullptr || (owner && (owner_sid == nullptr || !is_trusted_sid(owner_sid)));
    for (DWORD i = 0; !unsafe && i < dacl->AceCount; ++i) {
      void *entry = nullptr;
      if (!GetAce(dacl, i, &entry) || entry == nullptr) continue;
      const auto *header = static_cast<ACE_HEADER *>(entry);
      if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERIT_ONLY_ACE)) continue;
      const auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(entry);
      if (is_trusted_sid(reinterpret_cast<PSID>(const_cast<DWORD *>(&ace->SidStart)))) continue;
      if (ace->Mask & (owner ? (kOnItself | kInIt) : kInIt)) unsafe = true;
    }
    LocalFree(descriptor);
    return unsafe;
  }

  /**
   * Every existing folder between the root of the drive and the install folder (the install folder itself is given its own protection): `others_can_replace`. The root is looked
   * at for the right to delete what lies in it only. The folders that are not there yet are created protected by prepare(): nothing to look at.
   */
  inline bool
  parents_unsafe(const std::wstring &path) {
    if (others_can_replace(path.substr(0, 3), false)) return true;
    for (size_t at = 3; at < path.size(); ++at) {
      if (path[at] != L'\\') continue;
      const std::wstring ancestor = path.substr(0, at);
      if (GetFileAttributesW(ancestor.c_str()) == INVALID_FILE_ATTRIBUTES) return false;  // from here on it is ours to create
      if (others_can_replace(ancestor, true)) return true;
    }
    return false;
  }

  /** The drive is a fixed disk of this PC (not a mapped network drive, a removable one, or a subst drive). Asked before the folder is touched: a network drive that does not answer must not hold the window up. */
  inline bool
  fixed_drive(const wchar_t drive) {
    const std::wstring root = std::wstring(1, drive) + L":\\";
    return GetDriveTypeW(root.c_str()) == DRIVE_FIXED && !is_substituted(drive);
  }

  /** What a folder holds, for the rules and for a picked folder: whether it exists as a directory, is empty, is an install of ours, could be listed. */
  struct folder_state {
    bool exists = false;
    bool is_directory = false;
    bool empty = false;
    bool accessible = true;
    bool ours = false;
  };

  inline folder_state
  inspect_folder(const std::wstring &path) {
    folder_state f;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    f.exists = attributes != INVALID_FILE_ATTRIBUTES;
    f.is_directory = f.exists && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    if (!f.is_directory) return f;
    bool leftover_settings = false;
    WIN32_FIND_DATAW entry {};
    HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) {
      f.accessible = false;
    }
    else {
      f.empty = true;
      bool only_settings = true;  // nothing but the `config` folder: what an uninstall that kept the person's data leaves behind
      bool more = true;
      while (more) {
        const std::wstring_view name = entry.cFileName;
        if (name != L"." && name != L"..") {
          f.empty = false;
          if (!installer_dir::same_text(name, L"config") || !(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) only_settings = false;
        }
        more = FindNextFileW(find, &entry) != FALSE;
      }
      // only "no more files" is the end of the list; any other error means that the folder was not read to the end, and it is not looked at as empty or as an old install
      if (GetLastError() != ERROR_NO_MORE_FILES) f.accessible = false;
      FindClose(find);
      leftover_settings = f.accessible && !f.empty && only_settings;
    }
    const std::wstring registered = registered_install_dir();
    f.ours = (!registered.empty() && installer_dir::same_text(registered, path)) || (file_exists(path + L"\\sunshine.exe") && file_exists(path + L"\\unins000.exe")) || leftover_settings;
    return f;
  }

  /** Everything installer_dir::check needs to know, read from this PC. `path` is normalised and passed installer_dir::look. */
  inline installer_dir::facts
  gather(const std::wstring &path, const wchar_t drive) {
    installer_dir::facts f;
    const std::wstring root = std::wstring(1, drive) + L":\\";
    f.fixed_disk = fixed_drive(drive);
    wchar_t fs[64] {};
    if (GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fs, static_cast<DWORD>(std::size(fs)))) f.file_system = fs;
    const std::wstring there = nearest_existing(path);
    ULARGE_INTEGER free_to_caller {}, total {}, free_total {};
    if (GetDiskFreeSpaceExW(there.c_str(), &free_to_caller, &total, &free_total)) f.free_bytes = free_to_caller.QuadPart;

    // the folder, and every folder on the way: a junction or a symbolic link anywhere sends the program somewhere else
    const folder_state state = inspect_folder(path);
    f.exists = state.exists;
    f.is_directory = state.is_directory;
    f.empty = state.empty;
    f.accessible = state.accessible;
    f.ours = state.ours;
    for (size_t at = 3; at <= path.size(); ++at) {
      if (at != path.size() && path[at] != L'\\') continue;
      const DWORD a = GetFileAttributesW(path.substr(0, at).c_str());
      if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) f.reparse_point = true;
    }

    f.parent_unsafe = !under_program_files(path) && parents_unsafe(path);

    // places that may not hold the program
    wchar_t windows[MAX_PATH] {};
    if (GetWindowsDirectoryW(windows, MAX_PATH)) f.system_roots.push_back(installer_dir::normalise(windows));
    const std::wstring drive_root = std::wstring(1, drive) + L":";
    for (const wchar_t *name : { L"\\$Recycle.Bin", L"\\System Volume Information", L"\\Recovery", L"\\$WinREAgent" }) f.system_roots.push_back(drive_root + name);
    f.system_roots.push_back(program_files() + L"\\WindowsApps");
    for (const KNOWNFOLDERID *id : { &FOLDERID_UserProfiles, &FOLDERID_Profile, &FOLDERID_Public, &FOLDERID_Desktop, &FOLDERID_Documents, &FOLDERID_Downloads }) {
      const std::wstring dir = known_folder(*id);
      if (!dir.empty()) f.user_roots.push_back(long_form(dir));  // in the same form (long names, the case on disk) as the candidate
    }
    for (const wchar_t *name : { L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial" }) {
      const std::wstring dir = env_value(name);
      if (!dir.empty()) f.synced_roots.push_back(long_form(installer_dir::normalise(dir)));
    }
    f.synced_names = installer_dir::default_synced_names();
    return f;
  }

  /** The answer for one path: the text first, the machine only when the text is a drive path. */
  inline installer_dir::verdict
  verdict_for(const std::wstring &raw) {
    const installer_dir::shape s = installer_dir::look(raw);
    if (s.drive == 0 || (s.problem != installer_dir::error::none && s.problem != installer_dir::error::too_long)) return { s.problem, installer_dir::error::none, s.path };
    if (!fixed_drive(s.drive)) return { installer_dir::error::not_fixed, installer_dir::error::none, s.path };  // nothing of a network or removable drive is read
    const std::wstring path = long_form(s.path);
    return installer_dir::check(path, gather(path, s.drive));
  }

  /** Free bytes on the drive of the folder (null when unknown), for the line under the path. */
  inline std::optional<std::uint64_t>
  free_bytes_of(const std::wstring &path) {
    const installer_dir::shape s = installer_dir::look(path);
    if (s.drive == 0 || !fixed_drive(s.drive)) return std::nullopt;
    ULARGE_INTEGER free_to_caller {}, total {}, free_total {};
    if (!GetDiskFreeSpaceExW(nearest_existing(s.path).c_str(), &free_to_caller, &total, &free_total)) return std::nullopt;
    return free_to_caller.QuadPart;
  }

  /**
   * What the window says about the folder it shows: the rules of verdict_for, except the default folder. That one is the engine's own choice and keeps the behaviour it always had
   * (its folder may hold an older copy): only its room is looked at. The window and --selftest-dir both ask this, so they never disagree.
   */
  inline installer_dir::verdict
  verdict_for_window(const std::wstring &raw) {
    const std::wstring path = installer_dir::normalise(raw);
    if (installer_dir::same_text(path, default_dir())) {
      installer_dir::verdict v;
      v.path = path;
      const auto free = free_bytes_of(path);
      if (free && *free < installer_dir::kNeededBytes) v.problem = installer_dir::error::no_space;
      return v;
    }
    return verdict_for(raw);
  }

  /**
   * The install folder for a folder the person picked in the system picker (installer_dir::from_pick): the product's folder name is added, except to an install of ours, the
   * default folder and a folder that already has that name. The picked folder is looked at once, with its drive asked first (a network drive that does not answer must not hold the
   * window up); a path that is not a drive path is passed on as it is and refused by the rules.
   */
  inline installer_dir::pick
  resolve_pick(const std::wstring &picked) {
    const installer_dir::shape s = installer_dir::look(picked);
    bool ours = false, occupied = false;
    std::wstring chosen = picked;
    if (s.drive != 0 && !s.root && fixed_drive(s.drive)) {
      // the folder in its long name: an 8.3 alias of a folder already named like the product must be seen as that name, and what is looked at is the same folder
      chosen = long_form(s.path);
      const folder_state state = inspect_folder(chosen);
      ours = state.ours;
      occupied = state.exists && state.is_directory && !state.empty && !state.ours;
    }
    return installer_dir::from_pick(chosen, ours, occupied, default_dir());
  }

  /** The system folder picker (FOS_PICKFOLDERS): the folder the person chose, empty when cancelled. `owner` is the window it opens over. */
  inline std::wstring
  pick_folder(HWND owner, const std::wstring &start, const wchar_t *title) {
    IFileOpenDialog *dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    std::wstring chosen;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    if (title && *title) dialog->SetTitle(title);
    if (!start.empty()) {
      IShellItem *item = nullptr;
      if (SUCCEEDED(SHCreateItemFromParsingName(nearest_existing(start).c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        dialog->SetFolder(item);
        item->Release();
      }
    }
    if (SUCCEEDED(dialog->Show(owner))) {
      IShellItem *result = nullptr;
      if (SUCCEEDED(dialog->GetResult(&result))) {
        PWSTR name = nullptr;
        if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
          chosen = installer_dir::normalise(name);
          CoTaskMemFree(name);
        }
        result->Release();
      }
    }
    dialog->Release();
    return chosen;
  }

  // ---------------------------------------------------------------- the elevated worker

  /** Turn a privilege on for this process (an elevated Administrator has SeRestore and SeTakeOwnership, off by default). */
  inline bool
  enable_privilege(const wchar_t *name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES privileges {};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid) && AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
  }

  /**
   * Set owner Administrators and the protected DACL on an existing folder (not its content). Win32 error, 0 = done.
   * The folder is opened once and pinned: a junction or link, or a path that leads somewhere else than it says, is refused, and the owner and DACL are set through
   * that handle, so that a folder swapped in between the check and the change cannot receive them.
   * Authority: installer-ms-createfilew-20261119 (FILE_FLAG_OPEN_REPARSE_POINT, FILE_FLAG_BACKUP_SEMANTICS), installer-ms-setsecurityinfo-20261119,
   * installer-ms-getfinalpathnamebyhandlew-20261119.
   */
  inline DWORD
  protect_existing(const std::wstring &path) {
    enable_privilege(SE_RESTORE_NAME);
    enable_privilege(SE_TAKE_OWNERSHIP_NAME);
    HANDLE dir = CreateFileW(path.c_str(), READ_CONTROL | WRITE_DAC | WRITE_OWNER, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (dir == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD status = ERROR_SUCCESS;
    BY_HANDLE_FILE_INFORMATION info {};
    if (!GetFileInformationByHandle(dir, &info)) {
      status = GetLastError();
    }
    else if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      status = ERROR_REPARSE_POINT_ENCOUNTERED;
    }
    else {
      wchar_t final_path[2048] {};
      const DWORD n = GetFinalPathNameByHandleW(dir, final_path, static_cast<DWORD>(std::size(final_path)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      std::wstring resolved = n > 0 && n < std::size(final_path) ? std::wstring(final_path, n) : std::wstring();
      if (resolved.starts_with(L"\\\\?\\")) resolved.erase(0, 4);
      if (resolved.empty() || !installer_dir::same_text(installer_dir::normalise(resolved), installer_dir::normalise(path))) status = ERROR_REPARSE_POINT_ENCOUNTERED;
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (status == ERROR_SUCCESS && !ConvertStringSecurityDescriptorToSecurityDescriptorW(kProtectedSddl, SDDL_REVISION_1, &descriptor, nullptr)) status = GetLastError();
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    if (status == ERROR_SUCCESS && !GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted)) status = GetLastError();
    PSID administrators = nullptr;
    if (status == ERROR_SUCCESS && !ConvertStringSidToSidW(L"BA", &administrators)) status = GetLastError();
    if (status == ERROR_SUCCESS)
      status = SetSecurityInfo(dir, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, administrators, nullptr, dacl, nullptr);
    if (administrators) LocalFree(administrators);
    if (descriptor) LocalFree(descriptor);
    CloseHandle(dir);
    return status;
  }

  /** CreateDirectory with the protected DACL from the first instant: no moment in which the new folder carries the parent's permissive ACL. */
  inline DWORD
  create_protected(const std::wstring &path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kProtectedSddl, SDDL_REVISION_1, &descriptor, nullptr)) return GetLastError();
    SECURITY_ATTRIBUTES attributes { sizeof(attributes), descriptor, FALSE };
    const bool made = CreateDirectoryW(path.c_str(), &attributes) != FALSE;
    const DWORD error = made ? ERROR_SUCCESS : GetLastError();
    LocalFree(descriptor);
    return error;
  }

  /**
   * The folder of the install, made ready by the elevated worker before the engine starts: checked again here (the worker never takes the shell's word), every
   * missing folder on the way created protected, an existing empty or already installed folder given the owner and DACL. nullopt = ready; otherwise the reason.
   * A warning (a folder above that others can replace, a user folder) is a reason too, unless `allow_warning`: the person confirmed it in the window, or the silent command
   * line said /ALLOWUNSAFEPARENT. The install folder itself is protected the same either way.
   * Below Program Files nothing is changed: the folder inherits that protection and the engine creates it.
   */
  inline std::optional<installer_dir::error>
  prepare(std::wstring &raw, DWORD *win32_error, const bool allow_warning = false) {
    *win32_error = 0;
    const installer_dir::verdict v = verdict_for(raw);
    if (v.problem != installer_dir::error::none) return v.problem;
    if (v.warning != installer_dir::error::none && !allow_warning) return v.warning;
    raw = v.path;  // the engine is given the checked path, in long names
    if (under_program_files(v.path)) return std::nullopt;
    // create the missing folders from the top down; one that appeared in the meantime was made by somebody else, and its permissions are not ours
    std::vector<std::wstring> missing;
    for (std::wstring p = v.path; p.size() > 3 && GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES;) {
      missing.push_back(p);
      p.resize(p.find_last_of(L'\\'));
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
      const DWORD e = create_protected(*it);
      if (e != ERROR_SUCCESS) {
        *win32_error = e;
        return installer_dir::error::no_access;
      }
    }
    if (!missing.empty()) {
      // look at the whole chain again now that it is there: a folder that somebody else made above the ones created here, between the check and the creation, is seen as the
      // existing folder it is and refused for what it is (the folders made here are protected and stay so)
      const installer_dir::verdict again = verdict_for(raw);
      if (again.problem != installer_dir::error::none) return again.problem;
    }
    if (missing.empty()) {
      // an existing folder: take it over through one pinned handle. Whatever came into it between the check and the new permissions means it was not empty after all:
      // the permissions are the ones every Sunshine folder has, so nothing is undone, and the install does not start.
      const DWORD e = protect_existing(v.path);
      if (e != ERROR_SUCCESS) {
        *win32_error = e;
        return installer_dir::error::no_access;
      }
      const installer_dir::verdict again = verdict_for(raw);
      if (again.problem != installer_dir::error::none) return again.problem;
    }
    return std::nullopt;
  }

}  // namespace installer_dir_win
