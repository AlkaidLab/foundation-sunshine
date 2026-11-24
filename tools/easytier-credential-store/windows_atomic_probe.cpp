// Disposable Windows-only experiment. This is NOT a production credential
// store: it accepts no paths, touches no existing configuration or ACL, and
// does not protect against another process with the same Windows identity.
#ifndef _WIN32
  #error This isolated experiment requires Windows.
#endif

#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  constexpr DWORD symlink_allow_unprivileged = 0x2;

  struct handle_t {
    HANDLE value = INVALID_HANDLE_VALUE;
    handle_t() = default;
    explicit handle_t(HANDLE h): value(h) {}
    handle_t(const handle_t &) = delete;
    handle_t &operator=(const handle_t &) = delete;
    handle_t(handle_t &&other) noexcept: value(other.value) { other.value = INVALID_HANDLE_VALUE; }
    handle_t &operator=(handle_t &&other) noexcept {
      if (this != &other) {
        if (valid()) CloseHandle(value);
        value = other.value;
        other.value = INVALID_HANDLE_VALUE;
      }
      return *this;
    }
    ~handle_t() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    bool valid() const { return value != INVALID_HANDLE_VALUE && value != nullptr; }
  };

  [[noreturn]] void
  fail(const char *operation) {
    throw std::runtime_error(std::string(operation) + " failed (Windows error " + std::to_string(GetLastError()) + ")");
  }

  std::wstring
  trim_trailing_slashes(std::wstring value) {
    while (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/')) value.pop_back();
    return value;
  }

  std::wstring
  final_name(HANDLE handle) {
    const DWORD needed = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (needed == 0) fail("GetFinalPathNameByHandleW");
    std::wstring result(needed, L'\0');
    const DWORD actual = GetFinalPathNameByHandleW(handle, result.data(), needed, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (actual == 0 || actual >= needed) fail("GetFinalPathNameByHandleW");
    result.resize(actual);
    return trim_trailing_slashes(result);
  }

  std::wstring
  expected_final_name(const fs::path &path) {
    auto value = trim_trailing_slashes(path.lexically_normal().wstring());
    if (value.size() < 3 || value[1] != L':' || value[2] != L'\\') throw std::runtime_error("Only local absolute drive paths are supported");
    return trim_trailing_slashes(L"\\\\?\\" + value);
  }

  void
  require_path(HANDLE handle, const fs::path &path) {
    const auto actual = final_name(handle);
    const auto expected = expected_final_name(path);
    if (CompareStringOrdinal(actual.c_str(), -1, expected.c_str(), -1, TRUE) != CSTR_EQUAL) {
      std::wcerr << L"Expected final path: " << expected << L"; got: " << actual << L"\n";
      throw std::runtime_error("Final handle path differs from the validated absolute path");
    }
  }

  struct file_id_t {
    DWORD volume;
    DWORD high;
    DWORD low;
    bool operator==(const file_id_t &) const = default;
  };

  BY_HANDLE_FILE_INFORMATION
  information(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info {};
    if (!GetFileInformationByHandle(handle, &info)) fail("GetFileInformationByHandle");
    return info;
  }

  file_id_t
  id_of(HANDLE handle) {
    const auto info = information(handle);
    return {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
  }

  bool
  normal_directory(HANDLE handle) {
    const auto info = information(handle);
    return (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
  }

  std::wstring
  random_hex() {
    std::array<unsigned char, 16> data {};
    if (BCryptGenRandom(nullptr, data.data(), static_cast<ULONG>(data.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
      throw std::runtime_error("Random name generation failed");
    }
    static constexpr wchar_t alphabet[] = L"0123456789abcdef";
    std::wstring result;
    for (const auto byte : data) {
      result.push_back(alphabet[byte >> 4]);
      result.push_back(alphabet[byte & 15]);
    }
    return result;
  }

  struct security_t {
    std::vector<unsigned char> token_user;
    std::vector<unsigned char> system_sid;
    std::vector<unsigned char> admin_sid;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes {};

    security_t() {
      HANDLE raw = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) fail("OpenProcessToken");
      handle_t token(raw);
      DWORD length = 0;
      GetTokenInformation(token.value, TokenUser, nullptr, 0, &length);
      if (length == 0) fail("GetTokenInformation size");
      token_user.resize(length);
      if (!GetTokenInformation(token.value, TokenUser, token_user.data(), length, &length)) fail("GetTokenInformation");
      system_sid.resize(SECURITY_MAX_SID_SIZE);
      admin_sid.resize(SECURITY_MAX_SID_SIZE);
      DWORD system_size = static_cast<DWORD>(system_sid.size()), admin_size = static_cast<DWORD>(admin_sid.size());
      if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid.data(), &system_size) ||
          !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin_sid.data(), &admin_size)) fail("CreateWellKnownSid");
      LPWSTR sid_text = nullptr;
      if (!ConvertSidToStringSidW(user_sid(), &sid_text)) fail("ConvertSidToStringSidW");
      const std::wstring sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + std::wstring(sid_text) + L")";
      LocalFree(sid_text);
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) fail("Create security descriptor");
      attributes = {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    }
    ~security_t() { if (descriptor) LocalFree(descriptor); }
    PSID user_sid() { return reinterpret_cast<TOKEN_USER *>(token_user.data())->User.Sid; }

    void verify(HANDLE handle) {
      PSID owner = nullptr;
      PACL dacl = nullptr;
      PSECURITY_DESCRIPTOR actual = nullptr;
      const DWORD result = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                            &owner, nullptr, &dacl, nullptr, &actual);
      if (result != ERROR_SUCCESS) { SetLastError(result); fail("GetSecurityInfo"); }
      bool valid = owner && dacl && (EqualSid(owner, user_sid()) || EqualSid(owner, system_sid.data()) || EqualSid(owner, admin_sid.data()));
      SECURITY_DESCRIPTOR_CONTROL control = 0;
      DWORD revision = 0;
      valid = valid && GetSecurityDescriptorControl(actual, &control, &revision) && (control & SE_DACL_PROTECTED);
      bool user_seen = false, system_seen = false, admin_seen = false;
      ACL_SIZE_INFORMATION size {};
      valid = valid && GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation);
      for (DWORD i = 0; valid && i < size.AceCount; ++i) {
        void *raw_ace = nullptr;
        if (!GetAce(dacl, i, &raw_ace)) { valid = false; break; }
        auto *header = static_cast<ACE_HEADER *>(raw_ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERITED_ACE)) { valid = false; break; }
        auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(raw_ace);
        PSID sid = &ace->SidStart;
        const bool user = EqualSid(sid, user_sid());
        const bool system = EqualSid(sid, system_sid.data());
        const bool admin = EqualSid(sid, admin_sid.data());
        if (!(user || system || admin) || (ace->Mask & FILE_ALL_ACCESS) != FILE_ALL_ACCESS) { valid = false; break; }
        user_seen |= user; system_seen |= system; admin_seen |= admin;
      }
      LocalFree(actual);
      if (!valid || !user_seen || !system_seen || !admin_seen) throw std::runtime_error("Created object has an unexpected owner or DACL");
    }
  };

  struct probe_t {
    security_t security;
    fs::path base;
    fs::path root;
    std::vector<handle_t> parents;
    handle_t root_handle;
    std::map<std::wstring, file_id_t> owned;
    std::map<std::wstring, file_id_t> owned_directories;
    bool created = false;
    bool cleaned = false;

    probe_t() {
      std::wstring temp(32768, L'\0');
      const DWORD length = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
      if (length == 0 || length >= temp.size()) fail("GetTempPathW");
      temp.resize(length);
      base = fs::path(trim_trailing_slashes(fs::absolute(fs::path(temp)).lexically_normal().wstring()));
      expected_final_name(base); // rejects non-local/relative paths before mutation
      std::vector<fs::path> chain;
      for (auto p = base; !p.empty();) {
        chain.push_back(p);
        const auto parent = p.parent_path();
        if (parent == p || parent.empty()) break;
        p = parent;
      }
      std::reverse(chain.begin(), chain.end());
      for (const auto &p : chain) {
        handle_t h(CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!h.valid()) fail("Open TEMP ancestor");
        if (!normal_directory(h.value)) {
          throw std::runtime_error("TEMP ancestor is not a normal directory");
        }
        require_path(h.value, p);
        parents.push_back(std::move(h));
      }
      if (GetDriveTypeW(base.root_path().c_str()) != DRIVE_FIXED) throw std::runtime_error("TEMP volume is not a fixed local drive");
      std::array<wchar_t, 64> filesystem {};
      DWORD filesystem_flags = 0;
      if (!GetVolumeInformationW(base.root_path().c_str(), nullptr, 0, nullptr, nullptr, &filesystem_flags,
                                 filesystem.data(), static_cast<DWORD>(filesystem.size()))) fail("GetVolumeInformationW");
      if (CompareStringOrdinal(filesystem.data(), -1, L"NTFS", -1, TRUE) != CSTR_EQUAL ||
          !(filesystem_flags & FILE_PERSISTENT_ACLS)) {
        throw std::runtime_error("TEMP must be on an NTFS volume supporting persistent ACLs");
      }
      root = (base / (L"sunshine-et-atomic-probe-" + random_hex())).lexically_normal();
      if (root.parent_path() != base || root.filename().wstring().find(L"sunshine-et-atomic-probe-") != 0) {
        throw std::runtime_error("Invalid disposable directory");
      }
      if (!CreateDirectoryW(root.c_str(), &security.attributes)) fail("Create disposable directory");
      created = true;
      try {
        root_handle = handle_t(CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL | DELETE,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!root_handle.valid()) fail("Open disposable directory");
        const auto info = information(root_handle.value);
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            info.dwVolumeSerialNumber != information(parents.back().value).dwVolumeSerialNumber) {
          throw std::runtime_error("Disposable directory is invalid or on a different volume");
        }
        require_path(root_handle.value, root);
        security.verify(root_handle.value);
      }
      catch (...) {
        // Only delete the still-empty directory by a verified handle. Never
        // clean up an unverified or replaced path by name.
        bool removed = false;
        if (root_handle.valid()) {
          try {
            const auto info = information(root_handle.value);
            require_path(root_handle.value, root);
            if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
              FILE_DISPOSITION_INFO disposition {TRUE};
              removed = SetFileInformationByHandle(root_handle.value, FileDispositionInfo, &disposition, sizeof(disposition)) != 0;
            }
          }
          catch (...) {}
        }
        if (!removed) std::wcerr << L"Unverified disposable directory may remain: " << root.wstring() << L"\n";
        throw;
      }
    }

    ~probe_t() {
      if (created && !cleaned) {
        // Cleanup is explicit in main, so an abnormal constructor/exception
        // cannot recursively delete a directory whose identity is uncertain.
        std::wcerr << L"Disposable directory may remain: " << root.wstring() << L"\n";
      }
    }

    fs::path path(const std::wstring &leaf) const {
      if (leaf.empty() || leaf == L"." || leaf == L".." || leaf.find_first_of(L"\\/:*") != std::wstring::npos) {
        throw std::runtime_error("Only an owned direct child name is allowed");
      }
      const auto candidate = (root / leaf).lexically_normal();
      if (candidate.parent_path() != root) throw std::runtime_error("Path escaped disposable directory");
      return candidate;
    }

    file_id_t check_file(HANDLE handle, const std::wstring &leaf) {
      const auto info = information(handle);
      if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
          info.nNumberOfLinks != 1 || info.dwVolumeSerialNumber != information(root_handle.value).dwVolumeSerialNumber) {
        throw std::runtime_error("Target is a directory, reparse point, hard link, or different volume");
      }
      require_path(handle, path(leaf));
      security.verify(handle);
      return id_of(handle);
    }

    handle_t open(const std::wstring &leaf, DWORD access, DWORD sharing, DWORD creation, SECURITY_ATTRIBUTES *attributes = nullptr) {
      handle_t h(CreateFileW(path(leaf).c_str(), access, sharing, attributes, creation,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr));
      if (!h.valid()) fail("Open disposable file");
      return h;
    }

    void create(const std::wstring &leaf, std::string_view content) {
      auto h = open(leaf, GENERIC_WRITE | FILE_READ_ATTRIBUTES | READ_CONTROL | DELETE, 0, CREATE_NEW, &security.attributes);
      owned.emplace(leaf, id_of(h.value));
      check_file(h.value, leaf);
      DWORD written = 0;
      if (!WriteFile(h.value, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) || written != content.size() ||
          !FlushFileBuffers(h.value)) fail("Write disposable file");
    }

    std::string read(const std::wstring &leaf) {
      auto h = open(leaf, GENERIC_READ | FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING);
      check_file(h.value, leaf);
      std::array<char, 512> data {};
      DWORD read_bytes = 0;
      if (!ReadFile(h.value, data.data(), static_cast<DWORD>(data.size()), &read_bytes, nullptr)) fail("Read disposable file");
      return {data.data(), read_bytes};
    }

    enum class fault_t { none, short_write, flush_failure, replacement_failure, swap_target };

    bool replace(const std::wstring &leaf, std::string_view content, fault_t fault) {
      try {
        auto old = open(leaf, FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING);
        const auto original_id = check_file(old.value, leaf);
        const std::wstring temp_leaf = L"candidate-" + random_hex() + L".tmp";
        auto candidate = open(temp_leaf, GENERIC_WRITE | FILE_READ_ATTRIBUTES | READ_CONTROL | DELETE, 0, CREATE_NEW, &security.attributes);
        owned.emplace(temp_leaf, id_of(candidate.value));
        bool renamed = false;
        struct discard_t {
          HANDLE handle;
          bool &renamed;
          ~discard_t() {
            if (!renamed) { FILE_DISPOSITION_INFO info {TRUE}; SetFileInformationByHandle(handle, FileDispositionInfo, &info, sizeof(info)); }
          }
        } discard {candidate.value, renamed};
        check_file(candidate.value, temp_leaf);
        const DWORD wanted = static_cast<DWORD>(content.size());
        const DWORD submitted = fault == fault_t::short_write ? wanted / 2 : wanted;
        DWORD written = 0;
        if (!WriteFile(candidate.value, content.data(), submitted, &written, nullptr) || written != wanted) return false;
        if (fault == fault_t::flush_failure || !FlushFileBuffers(candidate.value)) return false;
        if (fault == fault_t::replacement_failure) return false;
        if (fault == fault_t::swap_target) {
          const std::wstring saved = L"prior-owned.json";
          if (!MoveFileExW(path(leaf).c_str(), path(saved).c_str(), MOVEFILE_WRITE_THROUGH)) fail("Move own original for swap test");
          owned.erase(leaf);
          owned.emplace(saved, original_id);
          create(leaf, "attacker-sentinel");
        }
        auto current = open(leaf, FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING);
        if (!(check_file(current.value, leaf) == original_id)) return false;
        // Close our destination observation handles before attempting rename.
        // The destination can still be swapped in the ensuing gap; this
        // experiment makes no same-identity adversary guarantee.
        current = handle_t {};
        old = handle_t {};

        // Source handle stays open. This Windows environment rejected a
        // non-null RootDirectory with ERROR_INVALID_PARAMETER, so the probe
        // uses the verified absolute destination while holding ancestor/root
        // handles. This is NOT an atomic expected-target-file-ID condition;
        // same-identity last-moment races remain out of scope.
        require_path(root_handle.value, root);
        const auto destination = path(leaf).wstring();
        const auto bytes = destination.size() * sizeof(wchar_t);
        std::vector<unsigned char> storage(offsetof(FILE_RENAME_INFO, FileName) + bytes + sizeof(wchar_t));
        auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
        rename->ReplaceIfExists = TRUE;
        rename->RootDirectory = nullptr;
        rename->FileNameLength = static_cast<DWORD>(bytes);
        std::memcpy(rename->FileName, destination.data(), bytes);
        if (!SetFileInformationByHandle(candidate.value, FileRenameInfo, rename, static_cast<DWORD>(storage.size()))) {
          const auto error = GetLastError();
          std::cerr << "Handle rename failed (Windows error " << error << ")\n";
          return false;
        }
        renamed = true;
        const auto new_id = id_of(candidate.value);
        owned.erase(temp_leaf);
        owned.insert_or_assign(leaf, new_id);
        return true;
      }
      catch (const std::exception &error) {
        std::cerr << "Replacement rejected: " << error.what() << "\n";
        return false;
      }
    }

    bool cleanup() {
      if (!created || !root_handle.valid()) return false;
      try {
        require_path(root_handle.value, root);
        for (const auto &[leaf, expected] : owned) {
          handle_t h(CreateFileW(path(leaf).c_str(), FILE_READ_ATTRIBUTES | DELETE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
          if (!h.valid()) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) continue;
            return false;
          }
          if (!(id_of(h.value) == expected)) return false;
          require_path(h.value, path(leaf));
          FILE_DISPOSITION_INFO disposition {TRUE};
          if (!SetFileInformationByHandle(h.value, FileDispositionInfo, &disposition, sizeof(disposition))) return false;
        }
        for (const auto &[leaf, expected] : owned_directories) {
          handle_t h(CreateFileW(path(leaf).c_str(), FILE_READ_ATTRIBUTES | DELETE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
          if (!h.valid() || !(id_of(h.value) == expected)) return false;
          require_path(h.value, path(leaf));
          FILE_DISPOSITION_INFO disposition {TRUE};
          if (!SetFileInformationByHandle(h.value, FileDispositionInfo, &disposition, sizeof(disposition))) return false;
        }
        FILE_DISPOSITION_INFO disposition {TRUE};
        if (!SetFileInformationByHandle(root_handle.value, FileDispositionInfo, &disposition, sizeof(disposition))) return false;
        CloseHandle(root_handle.value);
        root_handle.value = INVALID_HANDLE_VALUE;
        const auto attributes = GetFileAttributesW(root.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES || (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)) return false;
        cleaned = true;
        return true;
      }
      catch (...) { return false; }
    }
  };

  void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
  }

  bool test_owned_junction(probe_t &probe) {
    const std::wstring leaf = L"junction-owned";
    const auto junction_path = probe.path(leaf);
    if (!CreateDirectoryW(junction_path.c_str(), &probe.security.attributes)) fail("Create owned junction directory");
    handle_t junction(CreateFileW(junction_path.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | READ_CONTROL | DELETE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!junction.valid()) {
      // Do not delete a directory whose identity could not be recorded.
      throw std::runtime_error("Could not open the owned junction directory for cleanup");
    }
    probe.owned_directories.emplace(leaf, id_of(junction.value));
    require_path(junction.value, junction_path);
    probe.security.verify(junction.value);
    struct mount_point_t {
      DWORD tag;
      WORD data_length;
      WORD reserved;
      WORD substitute_offset;
      WORD substitute_length;
      WORD print_offset;
      WORD print_length;
      wchar_t path[1];
    };
    const auto print_name = probe.root.wstring();
    const auto substitute = L"\\??\\" + print_name;
    const auto sub_bytes = substitute.size() * sizeof(wchar_t);
    const auto print_bytes = print_name.size() * sizeof(wchar_t);
    const auto data_length = 8 + sub_bytes + sizeof(wchar_t) + print_bytes + sizeof(wchar_t);
    if (data_length > std::numeric_limits<WORD>::max()) throw std::runtime_error("Junction path is too long");
    std::vector<unsigned char> storage(8 + data_length);
    auto *mount = reinterpret_cast<mount_point_t *>(storage.data());
    mount->tag = IO_REPARSE_TAG_MOUNT_POINT;
    mount->data_length = static_cast<WORD>(data_length);
    mount->substitute_offset = 0;
    mount->substitute_length = static_cast<WORD>(sub_bytes);
    mount->print_offset = static_cast<WORD>(sub_bytes + sizeof(wchar_t));
    mount->print_length = static_cast<WORD>(print_bytes);
    std::memcpy(mount->path, substitute.data(), sub_bytes);
    std::memcpy(reinterpret_cast<unsigned char *>(mount->path) + mount->print_offset, print_name.data(), print_bytes);
    DWORD returned = 0;
    if (!DeviceIoControl(junction.value, FSCTL_SET_REPARSE_POINT, storage.data(), static_cast<DWORD>(storage.size()),
                         nullptr, 0, &returned, nullptr)) {
      const auto error = GetLastError();
      std::cout << "SKIP: owned junction creation was refused (error " << error << ")\n";
      return false;
    }
    require((information(junction.value).dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0,
            "Owned junction lacks a reparse attribute");
    require(!normal_directory(junction.value), "Reparse directory was accepted as a normal path component");
    require_path(junction.value, junction_path);
    std::cout << "PASS: owned reparse directory is rejected by ancestor validation\n";
    return true;
  }
}  // namespace

int main() {
  try {
    probe_t probe;
    bool ok = false;
    try {
      constexpr std::string_view old_value = "{\"devices\":[]}";
      constexpr std::string_view new_value = "{\"devices\":[\"test-public-key\"]}";
      const std::wstring target = L"credentials.json";
      probe.create(target, old_value);
      for (const auto fault : {probe_t::fault_t::short_write, probe_t::fault_t::flush_failure, probe_t::fault_t::replacement_failure}) {
        require(!probe.replace(target, new_value, fault), "Injected failure was accepted");
        require(probe.read(target) == old_value, "Injected failure changed committed content");
      }
      std::cout << "PASS: simulated short-write, flush, and replacement failures preserve the old file\n";
      {
        auto locked = probe.open(target, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING);
        require(!probe.replace(target, new_value, probe_t::fault_t::none), "Replacement ignored a real sharing violation");
      }
      require(probe.read(target) == old_value, "Real replacement failure changed committed content");
      std::cout << "PASS: real destination sharing violation preserves the old file\n";
      require(probe.replace(target, new_value, probe_t::fault_t::none), "Same-volume replacement failed");
      require(probe.read(target) == new_value, "Replacement content mismatch");
      std::cout << "PASS: flushed same-volume replacement with held source and directory handles\n";
      const std::wstring alias = L"hardlink-owned.json";
      if (!CreateHardLinkW(probe.path(alias).c_str(), probe.path(target).c_str(), nullptr)) fail("Create owned hard link");
      probe.owned.emplace(alias, probe.owned.at(target));
      require(!probe.replace(target, old_value, probe_t::fault_t::none), "Hard-linked target was accepted");
      {
        auto hardlink = probe.open(alias, FILE_READ_ATTRIBUTES | DELETE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING);
        require(id_of(hardlink.value) == probe.owned.at(alias), "Hard-link identity changed");
        require_path(hardlink.value, probe.path(alias));
        FILE_DISPOSITION_INFO disposition {TRUE};
        require(SetFileInformationByHandle(hardlink.value, FileDispositionInfo, &disposition, sizeof(disposition)) != 0,
                "Removing owned test hard link failed");
      }
      probe.owned.erase(alias);
      require(probe.read(target) == new_value, "Hard-link rejection changed target");
      std::cout << "PASS: preexisting hard-linked target is rejected\n";
      const std::wstring link = L"symlink-owned.json";
      bool tested_reparse = false;
      if (CreateSymbolicLinkW(probe.path(link).c_str(), probe.path(target).c_str(), symlink_allow_unprivileged)) {
        auto h = probe.open(link, FILE_READ_ATTRIBUTES | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING);
        probe.owned.emplace(link, id_of(h.value));
        require(!probe.replace(link, old_value, probe_t::fault_t::none), "Reparse-point target was accepted");
        require(probe.read(target) == new_value, "Reparse rejection changed target");
        std::cout << "PASS: preexisting reparse-point target is rejected\n";
        tested_reparse = true;
      }
      else {
        std::cout << "SKIP: Windows did not permit creation of a disposable symbolic link (error " << GetLastError() << ")\n";
      }
      tested_reparse = test_owned_junction(probe) || tested_reparse;
      require(tested_reparse, "No real reparse point could be created for testing");
      require(!probe.replace(target, old_value, probe_t::fault_t::swap_target), "Changed target identity was overwritten");
      require(probe.read(target) == "attacker-sentinel" && probe.read(L"prior-owned.json") == new_value, "Swap detection altered owned files");
      std::cout << "PASS: changed destination file identity is rejected before replacement\n";
      ok = true;
    }
    catch (const std::exception &error) {
      std::cerr << "FAIL: " << error.what() << "\n";
    }
    const bool cleaned = probe.cleanup();
    std::cout << (cleaned ? "PASS: exact owned files removed and disposable directory verified absent\n" : "FAIL: cleanup incomplete; inspect printed disposable path\n");
    return ok && cleaned ? 0 : 1;
  }
  catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << "\n";
    return 1;
  }
}
