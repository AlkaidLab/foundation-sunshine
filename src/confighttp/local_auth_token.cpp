/**
 * @file src/confighttp/local_auth_token.cpp
 * @brief Per-process credential file and constant-time matcher.
 *
 * The Windows file DACL follows Microsoft Learn's DACL model: an explicit,
 * protected DACL names SYSTEM, BUILTIN\\Administrators and the active console
 * user rather than inheriting an installation-directory default
 * [ds5-ms-acl-20261107] [ds5-ms-set-entries-acl-20261107]. The Windows file is
 * created with that descriptor and replaced atomically using the documented
 * file APIs [ds5-ms-createfilew-20261107] [ds5-ms-movefileexw-20261107]. POSIX
 * files use owner-only mode bits [ds5-posix-chmod-20261107]. The value is
 * compared with OpenSSL's constant-time primitive
 * [ds5-openssl-crypto-memcmp-20261107]. A named pipe was considered, but its
 * documented default grants Everyone read access, so this file uses an
 * explicit DACL instead [ds5-ms-named-pipe-security-20261107].
 */

#include "local_auth_token.h"

#include "src/config.h"
#include "src/crypto.h"
#include "src/file_handler.h"
#include "src/logging.h"
#ifdef _WIN32
  #include "src/platform/windows/misc.h"
#endif
#include "src/utility.h"

#ifdef _WIN32
  #include <aclapi.h>
  #include <windows.h>
#else
  #include <sys/stat.h>
#endif

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <openssl/crypto.h>

namespace confighttp::local_auth_token {
  namespace {
    constexpr auto token_file_name = "local-auth.token";
    constexpr std::size_t token_bytes = 32;

    std::mutex token_mutex;
    std::string current_token;

#ifdef _WIN32
    struct user_identity_t {
      bool system = false;
      std::vector<BYTE> sid;
    };

    std::mutex monitor_mutex;
    std::condition_variable monitor_condition;
    bool monitor_stop = false;
    std::thread monitor_thread;
    user_identity_t current_identity;
#endif

    std::filesystem::path
    token_path() {
      const auto configured = file_handler::path_from_utf8(config::sunshine.config_file);
      if (configured.has_parent_path()) {
        return configured.parent_path() / token_file_name;
      }
      return std::filesystem::current_path() / token_file_name;
    }

    std::filesystem::path
    temporary_path(const std::filesystem::path &path) {
      const auto suffix = util::hex_vec(crypto::rand(16));
      return path.parent_path() / (path.filename().string() + ".tmp-" + suffix);
    }

#ifdef _WIN32
    struct security_material_t {
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      std::vector<BYTE> system_sid_storage;
      std::vector<BYTE> administrators_sid_storage;
      std::vector<BYTE> user_sid_storage;
      PSID system_sid = nullptr;
      PSID administrators_sid = nullptr;
      PSID user_sid = nullptr;
      PACL acl = nullptr;

      security_material_t() = default;
      security_material_t(const security_material_t &) = delete;
      security_material_t &
      operator=(const security_material_t &) = delete;

      ~security_material_t() {
        if (acl) LocalFree(acl);
        if (descriptor) LocalFree(descriptor);
      }
    };

    bool
    make_well_known_sid(WELL_KNOWN_SID_TYPE type, std::vector<BYTE> &storage, PSID &sid) {
      DWORD size = 0;
      CreateWellKnownSid(type, nullptr, nullptr, &size);
      if (size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
      storage.resize(size);
      sid = storage.data();
      return CreateWellKnownSid(type, nullptr, sid, &size) != FALSE;
    }

    bool
    sid_from_token(HANDLE token, std::vector<BYTE> &storage) {
      DWORD size = 0;
      GetTokenInformation(token, TokenUser, nullptr, 0, &size);
      if (size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
      std::vector<BYTE> token_data(size);
      if (!GetTokenInformation(token, TokenUser, token_data.data(), size, &size)) return false;
      const auto *user = reinterpret_cast<const TOKEN_USER *>(token_data.data());
      const DWORD sid_size = GetLengthSid(user->User.Sid);
      storage.resize(sid_size);
      std::memcpy(storage.data(), user->User.Sid, sid_size);
      return IsValidSid(storage.data()) != FALSE;
    }

    bool
    query_user_identity(user_identity_t &identity) {
      // A SYSTEM service may query the physical console token only through
      // the WTS APIs; a user-mode process uses its own TokenUser SID
      // [ds5-ms-wts-active-20261107] [ds5-ms-wts-query-20261107]
      // [ds5-ms-token-user-20261107].
      identity.system = platf::is_running_as_system();
      HANDLE token = nullptr;
      if (identity.system) {
        token = platf::retrieve_users_token(false);
        if (!token) {
          // A service can start before a console user logs on.  In that state
          // the DACL intentionally contains only SYSTEM and Administrators.
          identity.sid.clear();
          return true;
        }
      }
      else if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
      }

      const bool ok = sid_from_token(token, identity.sid);
      CloseHandle(token);
      return ok;
    }

    bool
    make_security_material(security_material_t &material, const user_identity_t &identity) {
      if (!make_well_known_sid(WinLocalSystemSid, material.system_sid_storage, material.system_sid) ||
          !make_well_known_sid(WinBuiltinAdministratorsSid,
            material.administrators_sid_storage,
            material.administrators_sid)) {
        return false;
      }
      if (!identity.sid.empty()) {
        material.user_sid_storage = identity.sid;
        material.user_sid = material.user_sid_storage.data();
      }

      std::vector<EXPLICIT_ACCESSW> entries;
      const auto add_entry = [&entries](PSID sid, DWORD permissions) {
        EXPLICIT_ACCESSW entry {};
        entry.grfAccessPermissions = permissions;
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = NO_INHERITANCE;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
        entries.push_back(entry);
      };
      add_entry(material.system_sid, GENERIC_ALL);
      add_entry(material.administrators_sid, GENERIC_READ);
      if (material.user_sid && !EqualSid(material.user_sid, material.administrators_sid)) {
        add_entry(material.user_sid, GENERIC_READ);
      }

      if (SetEntriesInAclW(static_cast<ULONG>(entries.size()), entries.data(), nullptr, &material.acl) != ERROR_SUCCESS) {
        return false;
      }

      material.descriptor = LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
      if (!material.descriptor ||
          !InitializeSecurityDescriptor(material.descriptor, SECURITY_DESCRIPTOR_REVISION) ||
          !SetSecurityDescriptorDacl(material.descriptor, TRUE, material.acl, FALSE)) {
        return false;
      }

      if (!SetSecurityDescriptorControl(material.descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
        return false;
      }
      return true;
    }

    bool
    write_token_file(const std::filesystem::path &path, const std::string &token, const user_identity_t &identity) {
      security_material_t material;
      if (!make_security_material(material, identity)) {
        BOOST_LOG(error) << "Could not create local Web UI token security descriptor";
        return false;
      }

      SECURITY_ATTRIBUTES attributes {
        sizeof(SECURITY_ATTRIBUTES),
        material.descriptor,
        FALSE,
      };
      const auto temporary = temporary_path(path);
      const auto handle = CreateFileW(temporary.c_str(),
        GENERIC_WRITE,
        0,
        &attributes,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
      if (handle == INVALID_HANDLE_VALUE) {
        return false;
      }

      DWORD written = 0;
      const auto write_ok = WriteFile(handle,
                              token.data(),
                              static_cast<DWORD>(token.size()),
                              &written,
                              nullptr) &&
                            written == token.size() &&
                            FlushFileBuffers(handle);
      CloseHandle(handle);
      if (!write_ok ||
          !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
      }
      return true;
    }
#else
    bool
    write_token_file(const std::filesystem::path &path, const std::string &token) {
      const auto temporary = temporary_path(path);
      {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(token.data(), static_cast<std::streamsize>(token.size()));
        output.flush();
        if (!output) {
          std::error_code ignored;
          std::filesystem::remove(temporary, ignored);
          return false;
        }
      }

      std::error_code permission_error;
      std::filesystem::permissions(temporary,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace,
        permission_error);
      if (permission_error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
      }

      std::error_code rename_error;
      std::filesystem::rename(temporary, path, rename_error);
      if (rename_error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
      }
      return ::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0;
    }
#endif
  }  // namespace

#ifdef _WIN32
  namespace {
    bool
    same_identity(const user_identity_t &left, const user_identity_t &right) {
      return left.system == right.system && left.sid == right.sid;
    }

    bool
    rotate_token_locked(const user_identity_t &identity) {
      const auto random = crypto::rand(token_bytes);
      if (random.size() != token_bytes) return false;
      const auto token = util::hex_vec(random);
      const auto path = token_path();
      std::error_code directory_error;
      std::filesystem::create_directories(path.parent_path(), directory_error);
      if (directory_error || !write_token_file(path, token, identity)) return false;
      current_identity = identity;
      current_token = token;
      return true;
    }

    void
    refresh_user_identity() {
      user_identity_t identity;
      if (!query_user_identity(identity)) {
        std::lock_guard lock(token_mutex);
        current_token.clear();
        BOOST_LOG(error) << "Could not determine the Windows token file user; local API access is disabled";
        return;
      }

      std::lock_guard lock(token_mutex);
      if (same_identity(identity, current_identity)) return;
      current_token.clear();
      if (!rotate_token_locked(identity)) {
        BOOST_LOG(error) << "Could not rotate the local Web UI token after the console user changed";
      }
    }
  }  // namespace
#endif

  bool
  initialize() {
    std::lock_guard lock(token_mutex);
#ifdef _WIN32
    user_identity_t identity;
    if (!query_user_identity(identity)) {
      current_token.clear();
      BOOST_LOG(error) << "Could not determine the Windows token file user";
      return false;
    }
#endif
    const auto random = crypto::rand(token_bytes);
    if (random.size() != token_bytes) {
      current_token.clear();
      BOOST_LOG(error) << "Could not generate the local Web UI token";
      return false;
    }

    const auto token = util::hex_vec(random);
    const auto path = token_path();
    std::error_code directory_error;
    std::filesystem::create_directories(path.parent_path(), directory_error);
    if (directory_error ||
#ifdef _WIN32
        !write_token_file(path, token, identity)
#else
        !write_token_file(path, token)
#endif
    ) {
      current_token.clear();
      BOOST_LOG(error) << "Could not publish the local Web UI token file";
      return false;
    }

    current_token = token;
#ifdef _WIN32
    current_identity = std::move(identity);
#endif
    return true;
  }

  bool
  write_restricted_file(const std::filesystem::path &path, const std::string_view contents) {
    if (path.empty()) return false;
#ifdef _WIN32
    user_identity_t identity;
    if (!query_user_identity(identity)) return false;
    return write_token_file(path, std::string { contents }, identity);
#else
    return write_token_file(path, std::string { contents });
#endif
  }

  void
  start_monitor() {
#ifdef _WIN32
    std::lock_guard lock(monitor_mutex);
    if (monitor_thread.joinable()) return;
    monitor_stop = false;
    monitor_thread = std::thread([] {
      std::unique_lock lock(monitor_mutex);
      while (!monitor_condition.wait_for(lock, std::chrono::seconds(3), [] { return monitor_stop; })) {
        lock.unlock();
        refresh_user_identity();
        lock.lock();
      }
    });
#endif
  }

  void
  stop_monitor() {
#ifdef _WIN32
    {
      std::lock_guard lock(monitor_mutex);
      monitor_stop = true;
    }
    monitor_condition.notify_all();
    if (monitor_thread.joinable()) monitor_thread.join();
#endif
  }

  bool
  matches(const std::string_view candidate) {
    std::lock_guard lock(token_mutex);
    return !current_token.empty() && candidate.size() == current_token.size() &&
           CRYPTO_memcmp(candidate.data(), current_token.data(), current_token.size()) == 0;
  }
}  // namespace confighttp::local_auth_token
