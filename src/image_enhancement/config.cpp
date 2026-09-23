/**
 * @file src/image_enhancement/config.cpp
 * @brief Enhancement configuration persistence, trusted versions and maintenance reservations.
 */
#include "config.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <new>
#include <sstream>
#include <utility>
#include <vector>

#include <boost/smart_ptr/atomic_shared_ptr.hpp>
#include <boost/smart_ptr/weak_ptr.hpp>
#include <boost/thread/lock_guard.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <openssl/evp.h>

#include "src/config.h"
#include "src/file_handler.h"
#include "src/logging_severity.h"
#ifdef SUNSHINE_RTX_VIDEO_ADAPTER
  #include "rtx_video_trust.h"
#endif
#ifdef SUNSHINE_DLSSNR_ADAPTER
  #include "dlssnr_trust.h"
#endif

#ifdef _WIN32
  #include <windows.h>
#endif

namespace image_enhancement {
  namespace {
    using json = nlohmann::json;
    namespace fs = std::filesystem;
    constexpr std::size_t MAX_DOCUMENT = 64 * 1024;

    struct config_invalid_t {};
    struct component_untrusted_t {};
    struct digest_failed_t {};
    struct digest_limit_exceeded_t {};

    struct backend_traits_t {
      std::string_view id;
      backend_capability_e capability;
      std::string_view subdir;
      std::string_view adapter_name;
      std::string_view runtime_name;
      // The TrueHDR trust catalog keys runtime versions by their digest; the
      // DLSS NR runtime is pinned from the persisted settings instead.
      bool runtime_digest_is_version;
    };

    constexpr backend_traits_t BACKEND_TRAITS[] = {
      { NVIDIA_RTX_VIDEO_BACKEND, backend_capability_e::hdr, "nvidia_rtx_video",
        NVIDIA_RTX_VIDEO_ADAPTER, NVIDIA_RTX_VIDEO_RUNTIME, true },
      { NVIDIA_DLSSNR_BACKEND, backend_capability_e::nr, "nvidia_dlssnr",
        NVIDIA_DLSSNR_ADAPTER, NVIDIA_DLSSNR_RUNTIME, false },
    };

    const backend_traits_t &
    traits_for(std::string_view id) {
      for (const auto &traits : BACKEND_TRAITS) {
        if (traits.id == id) return traits;
      }
      throw component_untrusted_t {};
    }

    bool
    is_known_backend(std::string_view id) {
      for (const auto &traits : BACKEND_TRAITS) {
        if (traits.id == id) return true;
      }
      return false;
    }

    bool
    is_hex_digest(std::string_view value) {
      return value.size() == 64 &&
             value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
    }

    template<typename T, typename... Args>
    boost::shared_ptr<const T>
    make_immutable(Args &&...args) {
      return boost::shared_ptr<const T>(new T(std::forward<Args>(args)...));
    }

    struct validated_t {
      boost::shared_ptr<const backend_use_t> hdr;
      boost::shared_ptr<const backend_use_t> nr;
    };

    class maintenance_lock_t {
    public:
      explicit maintenance_lock_t(fs::path journal) {
        journal += ".lock";
#ifdef _WIN32
        handle_ = CreateFileW(journal.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        file_.open(journal, std::ios::app);
#endif
      }
      ~maintenance_lock_t() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#endif
      }
      explicit
      operator bool() const {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return file_.is_open();
#endif
      }
      maintenance_lock_t(const maintenance_lock_t &) = delete;
      maintenance_lock_t &
      operator=(const maintenance_lock_t &) = delete;

    private:
#ifdef _WIN32
      HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
      std::ofstream file_;
#endif
    };

    json
    read_document(const fs::path &path, bool missing_allowed = false) {
      if (missing_allowed && !fs::exists(path)) return json(json::value_t::discarded);
      std::ifstream stream(path, std::ios::binary);
      if (!stream) throw std::runtime_error("document_unreadable");
      std::string content(MAX_DOCUMENT + 1, '\0');
      stream.read(content.data(), static_cast<std::streamsize>(content.size()));
      const auto length = stream.gcount();
      if (stream.bad() || length <= 0 || length > MAX_DOCUMENT) throw std::runtime_error("document_invalid");
      content.resize(static_cast<std::size_t>(length));
      return json::parse(content);
    }

    std::string
    digest(std::istream &stream, std::uintmax_t maximum = MAX_DOCUMENT) {
      const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
      if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) throw digest_failed_t {};
      std::array<char, 64 * 1024> buffer {};
      std::uintmax_t consumed = 0;
      while (stream) {
        stream.read(buffer.data(), buffer.size());
        consumed += static_cast<std::uintmax_t>(stream.gcount());
        if (consumed > maximum) throw digest_limit_exceeded_t {};
        if (stream.gcount() > 0 && EVP_DigestUpdate(context.get(), buffer.data(), stream.gcount()) != 1) throw digest_failed_t {};
      }
      if (stream.bad()) throw digest_failed_t {};
      std::array<unsigned char, EVP_MAX_MD_SIZE> bytes {};
      unsigned int length = 0;
      if (EVP_DigestFinal_ex(context.get(), bytes.data(), &length) != 1) throw digest_failed_t {};
      std::ostringstream result;
      result << std::hex << std::setfill('0');
      for (unsigned int i = 0; i < length; ++i) result << std::setw(2) << static_cast<unsigned int>(bytes[i]);
      return result.str();
    }

    std::string
    entity_tag(const settings_t &settings) {
      std::istringstream stream(settings_json(settings).dump());
      return "\"hdr-v2-" + digest(stream) + "\"";
    }

    bool
    write_document(const fs::path &path, const json &value) {
      auto temporary = path;
      temporary += ".tmp";
      try {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << value.dump(2) << '\n';
        stream.flush();
        const bool written = stream.good();
        stream.close();
        if (!written || stream.fail()) throw std::runtime_error("write_failed");
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("replace_failed");
#else
        fs::rename(temporary, path);
#endif
        return true;
      }
      catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
      }
    }
  }  // namespace

  std::optional<backend_capability_e>
  backend_capability(std::string_view id) {
    for (const auto &traits : BACKEND_TRAITS) {
      if (traits.id == id) return traits.capability;
    }
    return std::nullopt;
  }

  bool
  valid_version(std::string_view version) {
    if (version.empty() || version.size() > 128 || version.front() == '.') return false;
    return version.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string_view::npos;
  }

  bool
  parse_settings(const nlohmann::json &input, settings_t &output) {
    try {
      if (!input.is_object() || input.size() != 3 || !input.at("schema_version").is_number_integer() ||
          !input.at("backends").is_object()) return false;
      const auto schema = input.at("schema_version").get<int>();
      if (schema != 1 && schema != 2) return false;
      settings_t parsed;
      if (schema == 1) {
        const auto &selected = input.at("selected_backend");
        if (!selected.is_null()) {
          if (!selected.is_string() || selected.get<std::string>() != std::string { NVIDIA_RTX_VIDEO_BACKEND }) return false;
          parsed.selected_backend = selected.get<std::string>();
        }
      }
      else {
        const auto &selected = input.at("selected");
        if (!selected.is_object() || selected.size() != 2 ||
            !selected.contains("hdr") || !selected.contains("nr")) return false;
        const auto &hdr = selected.at("hdr");
        const auto &nr = selected.at("nr");
        if (!hdr.is_null()) {
          if (!hdr.is_string() || hdr.get<std::string>() != std::string { NVIDIA_RTX_VIDEO_BACKEND }) return false;
          parsed.selected_backend = hdr.get<std::string>();
        }
        if (!nr.is_null()) {
          if (!nr.is_string() || nr.get<std::string>() != std::string { NVIDIA_DLSSNR_BACKEND }) return false;
          parsed.selected_nr_backend = nr.get<std::string>();
        }
      }
      for (const auto &[id, value] : input.at("backends").items()) {
        if (!is_known_backend(id) || !value.is_object() || value.empty() || value.size() > 2 ||
            !value.contains("version") || !value.at("version").is_string()) return false;
        const auto version = value.at("version").get<std::string>();
        if (!valid_version(version)) return false;
        if (value.contains("runtime_sha256")) {
          const auto &pin = value.at("runtime_sha256");
          if (!pin.is_null()) {
            if (!pin.is_string() || !is_hex_digest(pin.get<std::string>())) return false;
            parsed.runtime_pins.emplace(id, pin.get<std::string>());
          }
        }
        parsed.versions.emplace(id, version);
      }
      if (!parsed.selected_backend.empty() && !parsed.versions.contains(parsed.selected_backend)) return false;
      if (!parsed.selected_nr_backend.empty() && !parsed.versions.contains(parsed.selected_nr_backend)) return false;
      output = std::move(parsed);
      return true;
    }
    catch (const std::bad_alloc &) {
      throw;
    }
    catch (...) {
      return false;
    }
  }

  nlohmann::json
  settings_json(const settings_t &settings) {
    json backends = json::object();
    for (const auto &[id, version] : settings.versions) {
      json entry { { "version", version } };
      const auto pin = settings.runtime_pins.find(id);
      if (pin != settings.runtime_pins.end() && !pin->second.empty()) {
        entry["runtime_sha256"] = pin->second;
      }
      else {
        entry["runtime_sha256"] = nullptr;
      }
      backends[id] = entry;
    }
    return {
      { "schema_version", 2 },
      { "selected", {
        { "hdr", settings.selected_backend.empty() ? json(nullptr) : json(settings.selected_backend) },
        { "nr", settings.selected_nr_backend.empty() ? json(nullptr) : json(settings.selected_nr_backend) },
      } },
      { "backends", backends },
    };
  }

  struct manager_t::impl_t {
    fs::path file;
    fs::path root;
    json trust;
    fs::path maintenance_file;
    // 事务锁可覆盖文件 I/O；串流只使用独立的短使用权锁。
    boost::mutex transaction;
    boost::mutex ownership;
    boost::atomic_shared_ptr<const settings_t> active { make_immutable<settings_t>() };
    boost::atomic_shared_ptr<const backend_use_t> validated_hdr;
    boost::atomic_shared_ptr<const backend_use_t> validated_nr;
    std::vector<boost::weak_ptr<const backend_use_t>> users;
    std::string maintenance;
    std::string pending_removal;
    bool maintenance_unknown = false;

    settings_t
    disk() {
      try {
        const auto input = read_document(file, true);
        settings_t value;
        if (!input.is_discarded() && !parse_settings(input, value)) throw config_invalid_t {};
        return value;
      }
      catch (const std::bad_alloc &) {
        throw;
      }
      catch (const config_invalid_t &) {
        throw;
      }
      catch (...) {
        throw config_invalid_t {};
      }
    }

    // A loaded NGX module can keep its DLL mapped until this process exits.
    // Complete a previously authorized removal before validating or loading
    // any component in the new Sunshine process.
    bool
    complete_deferred_removal(std::string_view expected_id = {}, std::string_view expected_operation = {}) {
      const auto journal = read_document(maintenance_file, true);
      if (journal.is_discarded() || !journal.contains("pending_remove")) return false;
      const auto id = journal.at("pending_remove").get<std::string>();
      const auto operation = journal.at("operation_id").get<std::string>();
      if (operation.empty() || journal.at("component_id").get<std::string>() != id || !is_known_backend(id) ||
          (!expected_id.empty() && id != expected_id) ||
          (!expected_operation.empty() && operation != expected_operation)) {
        throw std::runtime_error("deferred_removal_invalid");
      }
      maintenance_lock_t operation_lock(maintenance_file);
      if (!operation_lock) throw std::runtime_error("deferred_removal_helper_running");
      if (read_document(maintenance_file) != journal) throw std::runtime_error("deferred_removal_changed");

      auto value = disk();
      const auto &traits = traits_for(id);
      const auto directory = root / "hdr_enhanced" / traits.subdir;
      if (fs::exists(directory) || fs::is_symlink(directory)) {
        const auto canonical_root = fs::canonical(root);
        if (canonical_root != fs::canonical(root.parent_path()) / root.filename())
          throw std::runtime_error("deferred_removal_root_invalid");
        const auto expected = canonical_root / "hdr_enhanced" / traits.subdir;
        if (fs::canonical(directory) != expected) throw std::runtime_error("deferred_removal_directory_invalid");
        for (const auto name : { traits.runtime_name, std::string_view { "component.json" } }) {
          const auto file = directory / name;
          if (fs::exists(file) || fs::is_symlink(file)) {
            if (!fs::is_regular_file(file) && !fs::is_symlink(file)) throw std::runtime_error("deferred_removal_file_invalid");
            fs::remove(file);
          }
        }
      }
      bool changed = false;
      if (value.selected_backend == id) {
        value.selected_backend.clear();
        changed = true;
      }
      if (value.selected_nr_backend == id) {
        value.selected_nr_backend.clear();
        changed = true;
      }
      changed |= value.versions.erase(std::string { id }) != 0;
      changed |= value.runtime_pins.erase(std::string { id }) != 0;
      if (changed && !write_document(file, settings_json(value))) throw std::runtime_error("deferred_removal_config_save_failed");
      if (!fs::remove(maintenance_file)) throw std::runtime_error("deferred_removal_journal_missing");
      BOOST_LOG(info) << "Completed deferred removal of " << id;
      return true;
    }

    /**
     * Validate one installed backend against the trust catalog and the
     * settings-carried runtime pin. The backend does not have to be selected.
     */
    boost::shared_ptr<const backend_use_t>
    validate_backend(const settings_t &value, std::string_view id) {
      try {
        const auto &traits = traits_for(id);
        const auto &version = value.versions.at(std::string { id });
        const auto directory = root / "hdr_enhanced" / traits.subdir;
        const auto &catalog = trust;
        if (!catalog.is_object() || catalog.value("schema_version", 0) != 1) throw component_untrusted_t {};
        const auto &trusted_adapter = catalog.at("adapters").at(std::string { traits.id }).at(std::string { traits.adapter_name });
        const auto canonical_directory = fs::canonical(directory);
        if (canonical_directory != fs::canonical(root) / "hdr_enhanced" / traits.subdir) throw component_untrusted_t {};
        const auto validate_file = [&](std::string_view name, const std::string &expected) {
          const auto path = fs::canonical(directory / std::string { name });
          if (path.parent_path() != canonical_directory || !fs::is_regular_file(path)) throw component_untrusted_t {};
          std::ifstream stream(path, std::ios::binary);
          constexpr auto maximum = 512ULL * 1024 * 1024;
          if (!stream || digest(stream, maximum) != expected) throw component_untrusted_t {};
        };
        validate_file(traits.adapter_name, trusted_adapter.get<std::string>());
        const auto pin = value.runtime_pins.find(std::string { id });
        const auto runtime_digest =
          pin != value.runtime_pins.end() ? pin->second : std::string {};
        if (traits.runtime_digest_is_version) {
          const auto &trusted_runtime =
            catalog.at("components").at(std::string { traits.id }).at(version).at(std::string { traits.runtime_name });
          validate_file(traits.runtime_name, trusted_runtime.get<std::string>());
        }
        else if (!runtime_digest.empty()) {
          validate_file(traits.runtime_name, runtime_digest);
        }
        else {
          // An unpinned runtime is accepted, but its in-place digest is
          // recorded so the actually loaded binary is identifiable.
          const auto path = fs::canonical(directory / std::string { traits.runtime_name });
          if (path.parent_path() != canonical_directory || !fs::is_regular_file(path)) throw component_untrusted_t {};
          std::ifstream stream(path, std::ios::binary);
          constexpr auto maximum = 512ULL * 1024 * 1024;
          if (!stream) throw component_untrusted_t {};
          BOOST_LOG(info) << "DLSS NR runtime is unpinned; recorded nvngx_dlssnr.dll sha256=" << digest(stream, maximum);
        }
        return make_immutable<backend_use_t>(backend_use_t { std::string { id }, version, canonical_directory / std::string { traits.adapter_name }, runtime_digest });
      }
      catch (const std::bad_alloc &) {
        throw;
      }
      catch (const digest_failed_t &) {
        throw;
      }
      catch (const digest_limit_exceeded_t &) {
        throw component_untrusted_t {};
      }
      catch (const component_untrusted_t &) {
        throw;
      }
      catch (...) {
        throw component_untrusted_t {};
      }
    }

    validated_t
    validate(const settings_t &value) {
      validated_t result;
      if (!value.selected_backend.empty()) {
        result.hdr = validate_backend(value, value.selected_backend);
      }
      if (!value.selected_nr_backend.empty()) {
        result.nr = validate_backend(value, value.selected_nr_backend);
      }
      return result;
    }

    bool
    used_locked() {
      std::erase_if(users, [](const auto &user) { return user.expired(); });
      return !users.empty();
    }
  };

  manager_t::manager_t(fs::path file, fs::path root, json trust):
      impl_(std::make_unique<impl_t>()) {
    // The GUI/elevated helper has its own working directory and requires an
    // absolute maintenance journal, even when Sunshine was given a relative config.
    impl_->file = fs::absolute(file).lexically_normal();
    impl_->root = std::move(root);
    impl_->trust = std::move(trust);
    impl_->maintenance_file = impl_->file.parent_path() / "hdr_enhanced.maintenance.json";
  }
  manager_t::~manager_t() = default;

  bool
  manager_t::initialize() {
    boost::lock_guard lock(impl_->transaction);
    try {
      impl_->complete_deferred_removal();
    }
    catch (const std::bad_alloc &) {
      throw;
    }
    catch (const std::exception &error) {
      BOOST_LOG(warning) << "Deferred enhancement removal remains pending: " << error.what();
    }
    catch (...) {
      BOOST_LOG(warning) << "Deferred enhancement removal remains pending: invalid configuration";
    }
    impl_->maintenance.clear();
    impl_->pending_removal.clear();
    impl_->maintenance_unknown = false;
    try {
      const auto maintenance = read_document(impl_->maintenance_file, true);
      if (!maintenance.is_discarded()) {
        impl_->maintenance = maintenance.at("operation_id").get<std::string>();
        if (impl_->maintenance.empty()) throw std::runtime_error("maintenance_invalid");
        if (maintenance.contains("pending_remove")) {
          const auto pending = maintenance.at("pending_remove").get<std::string>();
          if (!is_known_backend(pending) || maintenance.at("component_id").get<std::string>() != pending) {
            throw std::runtime_error("deferred_removal_invalid");
          }
          impl_->pending_removal = pending;
        }
      }
    }
    catch (...) {
      impl_->pending_removal.clear();
      impl_->maintenance_unknown = true;
    }
    try {
      const auto value = impl_->disk();
      impl_->active.store(make_immutable<settings_t>(value));
      const auto validated = impl_->validate(value);
      impl_->validated_hdr.store(validated.hdr);
      impl_->validated_nr.store(validated.nr);
      return true;
    }
    catch (...) {
      impl_->validated_hdr.store({});
      impl_->validated_nr.store({});
      return false;
    }
  }

  result_t
  manager_t::query() {
    boost::unique_lock lock(impl_->transaction, boost::try_to_lock);
    if (!lock.owns_lock()) return { 409, "hdr_save_busy" };
    try {
      const auto value = impl_->disk();
      return { 200, {}, value, entity_tag(value) };
    }
    catch (...) {
      return { 500, "hdr_config_invalid" };
    }
  }

  result_t
  manager_t::update(const settings_t &requested, std::optional<std::string_view> if_match,
    std::string_view operation_id) {
    boost::lock_guard lock(impl_->transaction);
    try {
      const auto previous = impl_->disk();
      const auto tag = entity_tag(previous);
      if (!if_match) return { 428, "hdr_precondition_required" };
      // The size and prefix guards must run before any substr: short values
      // would make substr throw and surface as a 500 instead of a 400.
      if (if_match->size() != tag.size() || !if_match->starts_with("\"hdr-v") ||
          if_match->back() != '"' ||
          if_match->substr(8, 64).find_first_not_of("0123456789abcdef") != std::string_view::npos) return { 400, "hdr_precondition_invalid" };
      const auto tag_version = if_match->substr(6, 2);
      if (tag_version != "1-" && tag_version != "2-") return { 400, "hdr_precondition_invalid" };
      if (*if_match != tag) return { 412, "hdr_config_changed" };
      // 助手持有文件锁直到安装和配置提交完成；配置事务不能反过来等待该锁。
      // 维护令牌限制提交者，结束维护仍须取得文件锁，不能越过正在写入的助手。
      settings_t checked;
      if (!parse_settings(settings_json(requested), checked)) return { 400, "hdr_config_invalid" };
      {
        boost::lock_guard gate(impl_->ownership);
        if (impl_->maintenance_unknown || ((!impl_->maintenance.empty() || !operation_id.empty()) && impl_->maintenance != operation_id)) return { 409, "hdr_component_busy" };
      }
      const bool selections_unchanged_and_valid =
        previous == requested && *impl_->active.load() == requested &&
        (requested.selected_backend.empty() || impl_->validated_hdr.load()) &&
        (requested.selected_nr_backend.empty() || impl_->validated_nr.load());
      if (operation_id.empty() && selections_unchanged_and_valid) {
        return { 200, {}, requested, tag, false };
      }
      const auto validated = impl_->validate(requested);
      if (!operation_id.empty() && previous.versions != requested.versions) {
        // 安装即使不启用增强，也必须验证待发布的版本；不能记录没有落地的 DLL。
        for (const auto &[id, version] : requested.versions) {
          if (id == requested.selected_backend || id == requested.selected_nr_backend) continue;
          impl_->validate_backend(requested, id);
        }
      }
      const auto snapshot = make_immutable<settings_t>(requested);
      const auto next_tag = entity_tag(requested);
      const bool changed = previous != requested;
      if (changed && !write_document(impl_->file, settings_json(requested))) return { 500, "hdr_save_failed" };
      {
        boost::lock_guard gate(impl_->ownership);
        impl_->validated_hdr.store(validated.hdr);
        impl_->validated_nr.store(validated.nr);
      }
      impl_->active.store(snapshot);
      return { 200, {}, requested, next_tag, changed };
    }
    catch (const config_invalid_t &) {
      return { 500, "hdr_config_invalid" };
    }
    catch (const component_untrusted_t &) {
      return { 400, "hdr_component_untrusted" };
    }
    catch (...) {
      return { 500, "hdr_save_failed" };
    }
  }

  boost::shared_ptr<const backend_use_t>
  manager_t::acquire_selected(backend_capability_e capability) {
    boost::lock_guard gate(impl_->ownership);
    if (!impl_->maintenance.empty() || impl_->maintenance_unknown) return {};
    boost::shared_ptr<const backend_use_t> backend;
    switch (capability) {
      case backend_capability_e::hdr:
        backend = impl_->validated_hdr.load();
        break;
      case backend_capability_e::nr:
        backend = impl_->validated_nr.load();
        break;
    }
    if (!backend) return {};
    // 每个会话独立拥有引用；配置快照本身不会被当作运行中的使用者。
    auto use = make_immutable<backend_use_t>(*backend);
    std::erase_if(impl_->users, [](const auto &user) { return user.expired(); });
    impl_->users.emplace_back(use);
    return use;
  }

  nlohmann::json
  manager_t::status() {
    std::error_code adapter_error;
    const bool adapter_present = fs::is_regular_file(
      impl_->root / "hdr_enhanced" / "nvidia_rtx_video" / NVIDIA_RTX_VIDEO_ADAPTER,
      adapter_error);
    std::error_code nr_adapter_error;
    const bool nr_adapter_present = fs::is_regular_file(
      impl_->root / "hdr_enhanced" / "nvidia_dlssnr" / NVIDIA_DLSSNR_ADAPTER,
      nr_adapter_error);
    boost::lock_guard gate(impl_->ownership);
    const auto settings = impl_->active.load();
    return { { "in_use", impl_->used_locked() }, { "maintenance", !impl_->maintenance.empty() || impl_->maintenance_unknown },
      { "pending_removal", impl_->pending_removal },
      { "adapter_present", adapter_present && !adapter_error },
      { "nr_adapter_present", nr_adapter_present && !nr_adapter_error },
      { "selected_backend", settings ? settings->selected_backend : std::string {} },
      { "selected_nr_backend", settings ? settings->selected_nr_backend : std::string {} },
      { "selection_verified", static_cast<bool>(impl_->validated_hdr.load()) },
      { "nr_selection_verified", static_cast<bool>(impl_->validated_nr.load()) },
      { "trusted_components", impl_->trust } };
  }

  std::filesystem::path
  manager_t::maintenance_path() const { return impl_->maintenance_file; }

  result_t
  manager_t::begin_maintenance(std::string_view id, std::string &operation_id) {
    if (!is_known_backend(id)) return { 404, "hdr_component_unknown" };
    boost::unique_lock lock(impl_->transaction, boost::try_to_lock);
    if (!lock.owns_lock()) return { 409, "hdr_save_busy" };
    const auto token = boost::uuids::to_string(boost::uuids::random_generator()());
    maintenance_lock_t operation_lock(impl_->maintenance_file);
    if (!operation_lock) return { 409, "hdr_helper_running" };
    {
      boost::lock_guard gate(impl_->ownership);
      if (impl_->used_locked() || !impl_->maintenance.empty() || impl_->maintenance_unknown) return { 409, "hdr_component_busy" };
      impl_->maintenance = token;
    }
    if (!write_document(impl_->maintenance_file, { { "operation_id", token }, { "component_id", id } })) {
      boost::lock_guard gate(impl_->ownership);
      impl_->maintenance.clear();
      return { 500, "hdr_maintenance_failed" };
    }
    operation_id = token;
    boost::lock_guard gate(impl_->ownership);
    impl_->validated_hdr.store({});
    impl_->validated_nr.store({});
    return {};
  }

  result_t
  manager_t::verify_maintenance(std::string_view id, std::string_view operation_id) {
    if (!is_known_backend(id) || operation_id.empty()) return { 400, "hdr_maintenance_invalid" };
    boost::lock_guard gate(impl_->ownership);
    if (impl_->maintenance_unknown || impl_->maintenance != operation_id) return { 409, "hdr_maintenance_mismatch" };
    return {};
  }

  result_t
  manager_t::defer_removal(std::string_view id, std::string_view operation_id) {
    if (!is_known_backend(id) || operation_id.empty()) return { 400, "hdr_maintenance_invalid" };
    boost::unique_lock lock(impl_->transaction, boost::try_to_lock);
    if (!lock.owns_lock()) return { 409, "hdr_save_busy" };
    maintenance_lock_t operation_lock(impl_->maintenance_file);
    if (!operation_lock) return { 409, "hdr_helper_running" };
    {
      boost::lock_guard gate(impl_->ownership);
      if (impl_->maintenance_unknown || impl_->maintenance != operation_id || !impl_->pending_removal.empty())
        return { 409, "hdr_maintenance_mismatch" };
    }
    try {
      const auto journal = read_document(impl_->maintenance_file);
      if (journal.at("operation_id").get<std::string>() != operation_id ||
          journal.at("component_id").get<std::string>() != id) return { 409, "hdr_maintenance_mismatch" };
      if (!write_document(impl_->maintenance_file, { { "operation_id", operation_id },
            { "component_id", id }, { "pending_remove", id } })) return { 500, "hdr_maintenance_failed" };
    }
    catch (...) {
      return { 500, "hdr_maintenance_failed" };
    }
    boost::lock_guard gate(impl_->ownership);
    impl_->pending_removal = id;
    return {};
  }

  result_t
  manager_t::inspect_maintenance(std::string_view id, std::string &operation_id) {
    if (!is_known_backend(id)) return { 404, "hdr_component_unknown" };
    boost::lock_guard gate(impl_->ownership);
    if (impl_->maintenance_unknown || impl_->maintenance.empty()) return { 409, "hdr_maintenance_mismatch" };
    operation_id = impl_->maintenance;
    return {};
  }

  result_t
  manager_t::finish_maintenance(std::string_view id, std::string_view operation_id) {
    if (!is_known_backend(id) || operation_id.empty()) return { 400, "hdr_maintenance_invalid" };
    boost::unique_lock lock(impl_->transaction, boost::try_to_lock);
    if (!lock.owns_lock()) return { 409, "hdr_save_busy" };
    // 与助手共用独占文件锁；锁释放后才能删除凭据，晚到的助手会因凭据失效拒绝写入。
    maintenance_lock_t operation_lock(impl_->maintenance_file);
    if (!operation_lock) return { 409, "hdr_helper_running" };
    {
      boost::lock_guard gate(impl_->ownership);
      if (impl_->maintenance != operation_id || impl_->maintenance_unknown) return { 409, "hdr_maintenance_mismatch" };
      if (!impl_->pending_removal.empty()) return { 409, "hdr_removal_pending" };
    }
    // 文件可能已被安装器替换；不能重新放行维护前验证过的路径快照。
    try {
      const auto value = impl_->disk();
      const auto validated = impl_->validate(value);
      impl_->active.store(make_immutable<settings_t>(value));
      impl_->validated_hdr.store(validated.hdr);
      impl_->validated_nr.store(validated.nr);
    }
    catch (const config_invalid_t &) {
      impl_->validated_hdr.store({});
      impl_->validated_nr.store({});
      return { 500, "hdr_config_invalid" };
    }
    catch (const component_untrusted_t &) {
      impl_->validated_hdr.store({});
      impl_->validated_nr.store({});
      return { 409, "hdr_component_untrusted" };
    }
    catch (...) {
      impl_->validated_hdr.store({});
      impl_->validated_nr.store({});
      return { 500, "hdr_maintenance_failed" };
    }
    std::error_code error;
    fs::remove(impl_->maintenance_file, error);
    if (error) return { 500, "hdr_maintenance_failed" };
    boost::lock_guard gate(impl_->ownership);
    impl_->maintenance.clear();
    return {};
  }

  result_t
  manager_t::recover_maintenance(std::string_view id) {
    if (!is_known_backend(id)) return { 404, "hdr_component_unknown" };
    boost::unique_lock lock(impl_->transaction, boost::try_to_lock);
    if (!lock.owns_lock()) return { 409, "hdr_save_busy" };
    std::string pending_operation;
    {
      boost::lock_guard gate(impl_->ownership);
      if (!impl_->pending_removal.empty()) {
        if (impl_->maintenance_unknown || impl_->pending_removal != id) return { 409, "hdr_maintenance_mismatch" };
        pending_operation = impl_->maintenance;
      }
    }
    if (!pending_operation.empty()) {
      try {
        if (!impl_->complete_deferred_removal(id, pending_operation)) return { 409, "hdr_maintenance_mismatch" };
        const auto value = impl_->disk();
        impl_->active.store(make_immutable<settings_t>(value));
        try {
          const auto validated = impl_->validate(value);
          impl_->validated_hdr.store(validated.hdr);
          impl_->validated_nr.store(validated.nr);
        }
        catch (...) {
          impl_->validated_hdr.store({});
          impl_->validated_nr.store({});
        }
        boost::lock_guard gate(impl_->ownership);
        impl_->maintenance.clear();
        impl_->pending_removal.clear();
        return {};
      }
      catch (const std::bad_alloc &) {
        throw;
      }
      catch (const std::exception &error) {
        BOOST_LOG(warning) << "Deferred enhancement removal retry failed: " << error.what();
        return { 500, "hdr_removal_pending" };
      }
      catch (...) {
        BOOST_LOG(warning) << "Deferred enhancement removal retry failed: invalid configuration";
        return { 500, "hdr_removal_pending" };
      }
    }
    maintenance_lock_t operation_lock(impl_->maintenance_file);
    if (!operation_lock) return { 409, "hdr_helper_running" };
    // 恢复不意味着强行启用。文件不匹配时保留用户设置，但禁用运行时引用，允许修复。
    impl_->validated_hdr.store({});
    impl_->validated_nr.store({});
    try {
      const auto value = impl_->disk();
      impl_->active.store(make_immutable<settings_t>(value));
      const auto validated = impl_->validate(value);
      impl_->validated_hdr.store(validated.hdr);
      impl_->validated_nr.store(validated.nr);
    }
    catch (...) {
      impl_->validated_hdr.store({});
      impl_->validated_nr.store({});
    }
    std::error_code error;
    fs::remove(impl_->maintenance_file, error);
    if (error) return { 500, "hdr_maintenance_failed" };
    boost::lock_guard gate(impl_->ownership);
    impl_->maintenance.clear();
    impl_->maintenance_unknown = false;
    return {};
  }

  manager_t &
  manager() {
    static manager_t instance(file_handler::path_from_utf8(config::sunshine.config_file).parent_path() / "hdr_enhanced.json",
      file_handler::path_from_utf8(SUNSHINE_ASSETS_DIR).parent_path() / "tools",
      [] {
        json catalog { { "schema_version", 1 }, { "components", json::object() } };
#ifdef SUNSHINE_RTX_VIDEO_ADAPTER
        catalog["adapters"][NVIDIA_RTX_VIDEO_BACKEND][NVIDIA_RTX_VIDEO_ADAPTER] = SUNSHINE_RTX_VIDEO_ADAPTER_SHA256;
        // 运行库版本仅由其完整摘要决定，不与适配器重编译时间绑定。
        catalog["components"][NVIDIA_RTX_VIDEO_BACKEND][SUNSHINE_RTX_VIDEO_RUNTIME_SHA256] = {
          { NVIDIA_RTX_VIDEO_RUNTIME, SUNSHINE_RTX_VIDEO_RUNTIME_SHA256 }
        };
#endif
#ifdef SUNSHINE_DLSSNR_ADAPTER
        catalog["adapters"][NVIDIA_DLSSNR_BACKEND][NVIDIA_DLSSNR_ADAPTER] = SUNSHINE_DLSSNR_ADAPTER_SHA256;
#endif
        return catalog;
      }());
    return instance;
  }
}  // namespace image_enhancement
