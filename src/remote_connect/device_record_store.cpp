#include "device_record_store.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#ifdef _WIN32
  #include <windows.h>
  #include <sddl.h>
#endif

namespace remote_connect::device_record_store {
  namespace {
    using credential_transaction::device_t;
    using credential_transaction::phase_t;
    using credential_transaction::public_key_t;
    using credential_transaction::record_t;
    using credential_transaction::transaction_id_t;

    // Bumped only when the meaning of a stored field changes. A record written
    // by another version is reported as invalid rather than guessed at.
    constexpr std::int64_t record_version = 1;
    // A record holds at most 1024 active and 1024 revoked devices, a few
    // hundred bytes each; anything larger is not a record of this host.
    constexpr std::size_t max_file_bytes = 1024 * 1024;

    template<std::size_t N>
    std::string
    to_hex(const std::array<std::uint8_t, N> &bytes) {
      static constexpr char digits[] = "0123456789abcdef";
      std::string out;
      out.reserve(bytes.size() * 2);
      for (const auto byte : bytes) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0f]);
      }
      return out;
    }

    template<std::size_t N>
    std::optional<std::array<std::uint8_t, N>>
    from_hex(const std::string &text) {
      const auto value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
      };
      if (text.size() != N * 2) return std::nullopt;
      std::array<std::uint8_t, N> out {};
      for (std::size_t index = 0; index < N; ++index) {
        const auto high = value(text[index * 2]);
        const auto low = value(text[index * 2 + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        out[index] = static_cast<std::uint8_t>(high * 16 + low);
      }
      return out;
    }

    struct phase_name_t {
      phase_t phase;
      const char *name;
    };

    // Every phase has a name; reduce() rejects anything outside this list.
    constexpr phase_name_t phase_names[] {
      { phase_t::stable, "stable" },
      { phase_t::add_prepared, "add_prepared" },
      { phase_t::add_verified_closed, "add_verified_closed" },
      { phase_t::add_committed_closed, "add_committed_closed" },
      { phase_t::add_rollback_closed, "add_rollback_closed" },
      { phase_t::revoke_pending, "revoke_pending" },
      { phase_t::revoke_verified_closed, "revoke_verified_closed" },
      { phase_t::revoke_committed_closed, "revoke_committed_closed" },
      { phase_t::recovery_closed, "recovery_closed" },
    };

    const char *
    name_of(phase_t phase) {
      for (const auto &entry : phase_names) {
        if (entry.phase == phase) return entry.name;
      }
      throw std::logic_error("unknown device record phase");
    }

    std::optional<phase_t>
    phase_from(const std::string &name) {
      for (const auto &entry : phase_names) {
        if (name == entry.name) return entry.phase;
      }
      return std::nullopt;
    }

    nlohmann::json
    device_json(const device_t &device) {
      nlohmann::json out = nlohmann::json::object();
      out["id"] = device.id;
      out["easytier_public_key"] = to_hex(device.easytier_public_key);
      out["gamestream_certificate_sha256"] = to_hex(device.gamestream_certificate_sha256);
      return out;
    }

    std::optional<device_t>
    device_from(const nlohmann::json &value) {
      if (!value.is_object()) return std::nullopt;
      device_t device;
      device.id = value.at("id").get<std::string>();
      const auto key = from_hex<32>(value.at("easytier_public_key").get<std::string>());
      const auto certificate = from_hex<32>(value.at("gamestream_certificate_sha256").get<std::string>());
      if (!key || !certificate) return std::nullopt;
      device.easytier_public_key = *key;
      device.gamestream_certificate_sha256 = *certificate;
      return device;
    }

    // Reads a list of devices into `into`. A repeated id, or one that is
    // already present, is not a record reduce() would ever have produced.
    template<typename Map>
    bool
    devices_from(const nlohmann::json &list, Map &into) {
      if (!list.is_array()) return false;
      for (const auto &entry : list) {
        const auto device = device_from(entry);
        if (!device) return false;
        if (!into.emplace(device->id, *device).second) return false;
      }
      return true;
    }

    std::optional<record_t>
    record_from(const nlohmann::json &document) {
      try {
        if (!document.is_object()) return std::nullopt;
        if (document.at("version").get<std::int64_t>() != record_version) return std::nullopt;

        record_t record;
        const auto &revision = document.at("revision");
        // A negative or fractional revision is not one this host ever wrote.
        if (!revision.is_number_unsigned()) return std::nullopt;
        record.revision = revision.get<std::uint64_t>();

        const auto phase = phase_from(document.at("phase").get<std::string>());
        if (!phase) return std::nullopt;
        record.phase = *phase;

        if (!devices_from(document.at("devices"), record.active)) return std::nullopt;
        if (!devices_from(document.at("revoked"), record.tombstones)) return std::nullopt;

        const auto &candidate = document.at("candidate");
        if (!candidate.is_null()) {
          const auto device = device_from(candidate);
          if (!device) return std::nullopt;
          record.candidate = *device;
        }

        const auto &revoke_target = document.at("revoke_target");
        if (!revoke_target.is_null()) record.revoke_target = revoke_target.get<std::string>();

        const auto transaction = from_hex<16>(document.at("transaction_id").get<std::string>());
        const auto last_transaction = from_hex<16>(document.at("last_transaction_id").get<std::string>());
        if (!transaction || !last_transaction) return std::nullopt;
        record.transaction_id = *transaction;
        record.last_transaction_id = *last_transaction;
        return record;
      }
      catch (...) {
        // A missing field or one of the wrong type: not a record of this host.
        return std::nullopt;
      }
    }

    nlohmann::json
    record_json(const record_t &record) {
      nlohmann::json active = nlohmann::json::array();
      for (const auto &[id, device] : record.active) {
        (void) id;
        active.push_back(device_json(device));
      }
      nlohmann::json revoked = nlohmann::json::array();
      for (const auto &[id, device] : record.tombstones) {
        (void) id;
        revoked.push_back(device_json(device));
      }

      nlohmann::json document = nlohmann::json::object();
      document["version"] = record_version;
      document["revision"] = record.revision;
      document["phase"] = name_of(record.phase);
      document["devices"] = std::move(active);
      document["revoked"] = std::move(revoked);
      document["candidate"] = record.candidate ? device_json(*record.candidate) : nlohmann::json();
      document["revoke_target"] = record.revoke_target ? nlohmann::json(*record.revoke_target) : nlohmann::json();
      document["transaction_id"] = to_hex(record.transaction_id);
      document["last_transaction_id"] = to_hex(record.last_transaction_id);
      return document;
    }

    bool
    write_all(const std::filesystem::path &path, const std::string &contents) {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      if (!out.is_open()) return false;
      out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
      out.flush();
      const bool written = out.good();
      out.close();
      return written && !out.fail();
    }

#ifdef _WIN32
    // The same protected DACL the credential store uses: SYSTEM,
    // administrators and the file's owner, and nobody else.
    DWORD
    restrict_access(const std::filesystem::path &path) {
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      constexpr auto sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)";
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        return GetLastError();
      }
      const BOOL ok = SetFileSecurityW(
        path.c_str(),
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        descriptor
      );
      const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
      LocalFree(descriptor);
      return error;
    }
#endif

    bool
    replace(const std::filesystem::path &temporary, const std::filesystem::path &path) {
#ifdef _WIN32
      return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
      std::error_code error;
      std::filesystem::rename(temporary, path, error);
      return !error;
#endif
    }

    void
    discard(const std::filesystem::path &temporary) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
    }
  }  // namespace

  load_result_t
  load(const std::filesystem::path &path) {
    // Ask whether the file exists rather than reading its status: with some
    // standard libraries a missing file is reported as "no status" with an
    // error, which would turn a host that has never enrolled a device into
    // one whose record cannot be read.
    std::error_code error;
    const bool present = std::filesystem::exists(path, error);
    if (error) return { load_status_e::unreadable, {} };
    if (!present) return { load_status_e::fresh, {} };
    const bool regular = std::filesystem::is_regular_file(path, error);
    if (error || !regular) return { load_status_e::unreadable, {} };

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return { load_status_e::unreadable, {} };
    std::string contents;
    char buffer[4096];
    while (in.read(buffer, sizeof buffer) || in.gcount() > 0) {
      contents.append(buffer, static_cast<std::size_t>(in.gcount()));
      if (contents.size() > max_file_bytes) return { load_status_e::invalid, {} };
    }
    if (in.bad()) return { load_status_e::unreadable, {} };

    const auto document = nlohmann::json::parse(contents, nullptr, false);
    if (document.is_discarded()) return { load_status_e::invalid, {} };
    const auto record = record_from(document);
    if (!record || !credential_transaction::valid(*record)) return { load_status_e::invalid, {} };
    return { load_status_e::loaded, *record };
  }

  bool
  store(const std::filesystem::path &path, const credential_transaction::record_t &record) {
    if (!credential_transaction::valid(record)) return false;
    const std::string contents = record_json(record).dump(2) + "\n";

    auto temporary = path;
    temporary += L".tmp";
    if (!write_all(temporary, contents)) {
      discard(temporary);
      return false;
    }
#ifdef _WIN32
    // Before the replace, so the record is never in place with wider access.
    if (restrict_access(temporary) != ERROR_SUCCESS) {
      discard(temporary);
      return false;
    }
#endif
    if (!replace(temporary, path)) {
      discard(temporary);
      return false;
    }
    return true;
  }
}  // namespace remote_connect::device_record_store
