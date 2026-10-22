/**
 * @file src/platform/windows/nvprefs/undo_file.cpp
 * @brief Definitions for the nvidia undo file.
 */
// local includes
#include "undo_file.h"

namespace {

  using namespace nvprefs;

  DWORD
  relax_permissions(HANDLE file_handle) {
    PACL old_dacl = nullptr;

    safe_hlocal<PSECURITY_DESCRIPTOR> sd;
    DWORD status = GetSecurityInfo(file_handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_dacl, nullptr, &sd);
    if (status != ERROR_SUCCESS) return status;

    safe_sid users_sid;
    SID_IDENTIFIER_AUTHORITY nt_authorithy = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&nt_authorithy, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_USERS, 0, 0, 0, 0, 0, 0, &users_sid)) {
      return GetLastError();
    }

    EXPLICIT_ACCESS ea = {};
    ea.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | DELETE;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.ptstrName = (LPTSTR) users_sid.get();

    safe_hlocal<PACL> new_dacl;
    status = SetEntriesInAcl(1, &ea, old_dacl, &new_dacl);
    if (status != ERROR_SUCCESS) return status;

    status = SetSecurityInfo(file_handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl.get(), nullptr);
    if (status != ERROR_SUCCESS) return status;

    return 0;
  }

}  // namespace

namespace nvprefs {

  std::optional<undo_file_t>
  undo_file_t::open_existing_file(std::filesystem::path file_path, bool &access_denied) {
    undo_file_t file;
    file.file_handle.reset(CreateFileW(file_path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (file.file_handle) {
      file.file_path = std::move(file_path);
      access_denied = false;
      return file;
    }
    else {
      auto last_error = GetLastError();
      access_denied = (last_error != ERROR_FILE_NOT_FOUND && last_error != ERROR_PATH_NOT_FOUND);
      return std::nullopt;
    }
  }

  std::optional<undo_file_t>
  undo_file_t::create_new_file(std::filesystem::path file_path) {
    undo_file_t file;
    file.file_handle.reset(CreateFileW(file_path.c_str(), GENERIC_WRITE | STANDARD_RIGHTS_ALL, FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL));

    if (file.file_handle) {
      file.file_path = std::move(file_path);
      // give GENERIC_READ, GENERIC_WRITE and DELETE permissions to Users group
      if (relax_permissions(file.file_handle.get()) != 0) {
        error_message("Failed to relax permissions on undo file");
      }
      return file;
    }
    else {
      return std::nullopt;
    }
  }

  bool
  undo_file_t::delete_file() {
    if (!file_handle) return false;

    FILE_DISPOSITION_INFO delete_file_info = { TRUE };
    if (SetFileInformationByHandle(file_handle.get(), FileDispositionInfo, &delete_file_info, sizeof(delete_file_info))) {
      file_handle.reset();
      return true;
    }
    else {
      return false;
    }
  }

  bool
  undo_file_t::write_undo_data(const undo_data_t &undo_data) {
    if (!file_handle) return false;

    std::string buffer;
    try {
      buffer = undo_data.write();
    }
    catch (...) {
      error_message("Couldn't serialize undo data");
      return false;
    }

    if (buffer.empty()) return false;

    // Keep the locked, committed journal intact until a complete replacement
    // has been flushed. FILE_SHARE_DELETE permits atomic replacement while
    // both handles continue denying other processes read/write access.
    auto temporary_path = file_path;
    temporary_path += L".tmp";
    safe_handle temporary(CreateFileW(temporary_path.c_str(), GENERIC_WRITE | STANDARD_RIGHTS_ALL,
      FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!temporary) {
      error_message("Couldn't create replacement undo file");
      return false;
    }
    auto discard_temporary = util::fail_guard([&]() {
      temporary.reset();
      DeleteFileW(temporary_path.c_str());
    });
    if (relax_permissions(temporary.get()) != ERROR_SUCCESS) {
      error_message("Couldn't prepare replacement undo file permissions");
      return false;
    }
    DWORD bytes_written = 0;
    if (!WriteFile(temporary.get(), buffer.data(), buffer.size(), &bytes_written, nullptr) || bytes_written != buffer.size()) {
      error_message("Couldn't write replacement undo file");
      return false;
    }
    if (!FlushFileBuffers(temporary.get())) {
      error_message("Failed to flush replacement undo file");
      return false;
    }
    const auto name = file_path.wstring();
    std::vector<char> rename_buffer(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(rename_buffer.data());
    rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    memcpy(rename->FileName, name.data(), rename->FileNameLength);
    // POSIX replacement allows the committed journal's lock handle to remain
    // open until the replacement handle takes over, on Windows 10 and newer.
    if (!SetFileInformationByHandle(temporary.get(), FileRenameInfoEx, rename, static_cast<DWORD>(rename_buffer.size()))) {
      const auto error = GetLastError();
      error_message("Couldn't atomically replace undo file: " + std::to_string(error));
      return false;
    }
    // The replacement is already locked: there is no close/reopen race.
    file_handle = std::move(temporary);
    discard_temporary.disable();

    return true;
  }

  std::optional<undo_data_t>
  undo_file_t::read_undo_data() {
    if (!file_handle) return std::nullopt;

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(file_handle.get(), &file_size)) {
      error_message("Couldn't get undo file size");
      return std::nullopt;
    }

    if ((size_t) file_size.QuadPart > 1024) {
      error_message("Undo file size is unexpectedly large, aborting");
      return std::nullopt;
    }

    std::vector<char> buffer(file_size.QuadPart);
    DWORD bytes_read = 0;
    if (!ReadFile(file_handle.get(), buffer.data(), buffer.size(), &bytes_read, nullptr) || bytes_read != buffer.size()) {
      error_message("Couldn't read undo file");
      return std::nullopt;
    }

    undo_data_t undo_data;
    try {
      if (!undo_data.read(buffer)) return std::nullopt;
    }
    catch (...) {
      error_message("Couldn't parse undo file");
      return std::nullopt;
    }
    return undo_data;
  }

}  // namespace nvprefs
