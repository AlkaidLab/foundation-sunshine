/**
 * @file src/hdr_enhanced/config.cpp
 * @brief HDR configuration persistence, trusted versions and maintenance reservations.
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
#ifdef SUNSHINE_RTX_VIDEO_ADAPTER
  #include "rtx_video_trust.h"
#endif

#ifdef _WIN32
  #include <windows.h>
#endif

namespace hdr_enhanced {
  namespace {
    using json = nlohmann::json;
    namespace fs = std::filesystem;
    constexpr std::size_t MAX_DOCUMENT = 64 * 1024;

    struct config_invalid_t {};
    struct component_untrusted_t {};
    struct digest_failed_t {};
    struct digest_limit_exceeded_t {};

    template<typename T, typename... Args>
    boost::shared_ptr<const T>
    make_immutable(Args &&...args) {
      return boost::shared_ptr<const T>(new T(std::forward<Args>(args)...));
    }

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
      return "\"hdr-v1-" + digest(stream) + "\"";
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

  bool
  valid_version(std::string_view version) {
    if (version.empty() || version.size() > 128 || version.front() == '.') return false;
    return version.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string_view::npos;
  }

  bool
  parse_settings(const nlohmann::json &input, settings_t &output) {
    try {
      if (!input.is_object() || input.size() != 3 || !input.at("schema_version").is_number_integer() || input.at("schema_version") != 1 ||
          !input.at("backends").is_object()) return false;
      settings_t parsed;
      const auto &selected = input.at("selected_backend");
      if (!selected.is_null()) {
        if (!selected.is_string() || selected.get<std::string>() != NVIDIA_RTX_VIDEO_BACKEND) return false;
        parsed.selected_backend = selected.get<std::string>();
      }
      for (const auto &[id, value] : input.at("backends").items()) {
        if (id != NVIDIA_RTX_VIDEO_BACKEND || !value.is_object() || value.size() != 1 || !value.at("version").is_string()) return false;
        const auto version = value.at("version").get<std::string>();
        if (!valid_version(version)) return false;
        parsed.versions.emplace(id, version);
      }
      if (!parsed.selected_backend.empty() && !parsed.versions.contains(parsed.selected_backend)) return false;
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
    for (const auto &[id, version] : settings.versions) backends[id] = { { "version", version } };
    return { { "schema_version", 1 }, { "selected_backend", settings.selected_backend.empty() ? json(nullptr) : json(settings.selected_backend) }, { "backends", backends } };
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
    boost::atomic_shared_ptr<const backend_use_t> validated;
    std::vector<boost::weak_ptr<const backend_use_t>> users;
    std::string maintenance;
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

    boost::shared_ptr<const backend_use_t>
    validate(const settings_t &value) {
      try {
        if (value.selected_backend.empty()) return {};
        const auto &version = value.versions.at(value.selected_backend);
        const auto directory = root / "hdr_enhanced" / "nvidia_rtx_video";
        const auto &catalog = trust;
        if (!catalog.is_object() || catalog.value("schema_version", 0) != 1) throw component_untrusted_t {};
        const auto &trusted = catalog.at("components").at(value.selected_backend).at(version);
        const auto &trusted_adapter = catalog.at("adapters").at(value.selected_backend);
        const auto canonical_directory = fs::canonical(directory);
        if (canonical_directory != fs::canonical(root) / "hdr_enhanced" / "nvidia_rtx_video") throw component_untrusted_t {};
        const auto validate_file = [&](const char *name, const std::string &expected) {
          const auto path = fs::canonical(directory / name);
          if (path.parent_path() != canonical_directory || !fs::is_regular_file(path)) throw component_untrusted_t {};
          std::ifstream stream(path, std::ios::binary);
          constexpr auto maximum = 512ULL * 1024 * 1024;
          if (!stream || digest(stream, maximum) != expected) throw component_untrusted_t {};
        };
        validate_file(NVIDIA_RTX_VIDEO_ADAPTER, trusted_adapter.at(NVIDIA_RTX_VIDEO_ADAPTER).get<std::string>());
        validate_file(NVIDIA_RTX_VIDEO_RUNTIME, trusted.at(NVIDIA_RTX_VIDEO_RUNTIME).get<std::string>());
        return make_immutable<backend_use_t>(backend_use_t { value.selected_backend, version, canonical_directory / NVIDIA_RTX_VIDEO_ADAPTER });
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

    bool
    used_locked() {
      std::erase_if(users, [](const auto &user) { return user.expired(); });
      return !users.empty();
    }
  };

  manager_t::manager_t(fs::path file, fs::path root, json trust):
      impl_(std::make_unique<impl_t>()) {
    impl_->file = std::move(file);
    impl_->root = std::move(root);
    impl_->trust = std::move(trust);
    impl_->maintenance_file = impl_->file.parent_path() / "hdr_enhanced.maintenance.json";
  }
  manager_t::~manager_t() = default;

  bool
  manager_t::initialize() {
    boost::lock_guard lock(impl_->transaction);
    try {
      const auto maintenance = read_document(impl_->maintenance_file, true);
      if (!maintenance.is_discarded()) {
        impl_->maintenance = maintenance.at("operation_id").get<std::string>();
        if (impl_->maintenance.empty()) throw std::runtime_error("maintenance_invalid");
      }
    }
    catch (...) {
      impl_->maintenance_unknown = true;
    }
    try {
      const auto value = impl_->disk();
      impl_->active.store(make_immutable<settings_t>(value));
      impl_->validated.store(impl_->validate(value));
      return true;
    }
    catch (...) {
      impl_->validated.store({});
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
      if (if_match->size() != tag.size() || !if_match->starts_with("\"hdr-v1-") || if_match->back() != '"' ||
          if_match->substr(8, 64).find_first_not_of("0123456789abcdef") != std::string_view::npos) return { 400, "hdr_precondition_invalid" };
      if (*if_match != tag) return { 412, "hdr_config_changed" };
      // 助手持有文件锁直到安装和配置提交完成；配置事务不能反过来等待该锁。
      // 维护令牌限制提交者，结束维护仍须取得文件锁，不能越过正在写入的助手。
      settings_t checked;
      if (!parse_settings(settings_json(requested), checked)) return { 400, "hdr_config_invalid" };
      {
        boost::lock_guard gate(impl_->ownership);
        if (impl_->maintenance_unknown || ((!impl_->maintenance.empty() || !operation_id.empty()) && impl_->maintenance != operation_id)) return { 409, "hdr_component_busy" };
      }
      if (operation_id.empty() && previous == requested && *impl_->active.load() == requested &&
          (requested.selected_backend.empty() || impl_->validated.load())) {
        return { 200, {}, requested, tag, false };
      }
      const auto backend = impl_->validate(requested);
      if (!operation_id.empty() && previous.versions != requested.versions) {
        // 安装即使不启用增强，也必须验证待发布的版本；不能记录没有落地的 DLL。
        for (const auto &[id, version] : requested.versions) {
          if (id == requested.selected_backend) continue;
          auto installed = requested;
          installed.selected_backend = id;
          impl_->validate(installed);
        }
      }
      const auto snapshot = make_immutable<settings_t>(requested);
      const auto next_tag = entity_tag(requested);
      const bool changed = previous != requested;
      if (changed && !write_document(impl_->file, settings_json(requested))) return { 500, "hdr_save_failed" };
      impl_->validated.store(backend);
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
  manager_t::acquire_selected() {
    boost::lock_guard gate(impl_->ownership);
    if (!impl_->maintenance.empty() || impl_->maintenance_unknown) return {};
    const auto backend = impl_->validated.load();
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
    boost::lock_guard gate(impl_->ownership);
    const auto settings = impl_->active.load();
    return { { "in_use", impl_->used_locked() }, { "maintenance", !impl_->maintenance.empty() || impl_->maintenance_unknown },
      { "adapter_present", adapter_present && !adapter_error },
      { "selected_backend", settings ? settings->selected_backend : std::string {} },
      { "selection_verified", static_cast<bool>(impl_->validated.load()) },
      { "trusted_components", impl_->trust } };
  }

  std::filesystem::path
  manager_t::maintenance_path() const { return impl_->maintenance_file; }

  result_t
  manager_t::begin_maintenance(std::string_view id, std::string &operation_id) {
    if (id != NVIDIA_RTX_VIDEO_BACKEND) return { 404, "hdr_component_unknown" };
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
    if (!write_document(impl_->maintenance_file, { { "operation_id", token } })) {
      boost::lock_guard gate(impl_->ownership);
      impl_->maintenance.clear();
      return { 500, "hdr_maintenance_failed" };
    }
    operation_id = token;
    impl_->validated.store({});
    return {};
  }

  result_t
  manager_t::verify_maintenance(std::string_view id, std::string_view operation_id) {
    if (id != NVIDIA_RTX_VIDEO_BACKEND || operation_id.empty()) return { 400, "hdr_maintenance_invalid" };
    boost::lock_guard gate(impl_->ownership);
    if (impl_->maintenance_unknown || impl_->maintenance != operation_id) return { 409, "hdr_maintenance_mismatch" };
    return {};
  }

  result_t
  manager_t::inspect_maintenance(std::string_view id, std::string &operation_id) {
    if (id != NVIDIA_RTX_VIDEO_BACKEND) return { 404, "hdr_component_unknown" };
    boost::lock_guard gate(impl_->ownership);
    if (impl_->maintenance_unknown || impl_->maintenance.empty()) return { 409, "hdr_maintenance_mismatch" };
    operation_id = impl_->maintenance;
    return {};
  }

  result_t
  manager_t::finish_maintenance(std::string_view id, std::string_view operation_id) {
    if (id != NVIDIA_RTX_VIDEO_BACKEND || operation_id.empty()) return { 400, "hdr_maintenance_invalid" };
    boost::unique_lock lock(impl_->transaction, boost::try_to_lock);
    if (!lock.owns_lock()) return { 409, "hdr_save_busy" };
    // 与助手共用独占文件锁；锁释放后才能删除凭据，晚到的助手会因凭据失效拒绝写入。
    maintenance_lock_t operation_lock(impl_->maintenance_file);
    if (!operation_lock) return { 409, "hdr_helper_running" };
    {
      boost::lock_guard gate(impl_->ownership);
      if (impl_->maintenance != operation_id || impl_->maintenance_unknown) return { 409, "hdr_maintenance_mismatch" };
    }
    // 文件可能已被安装器替换；不能重新放行维护前验证过的路径快照。
    try {
      const auto value = impl_->disk();
      const auto backend = impl_->validate(value);
      impl_->active.store(make_immutable<settings_t>(value));
      impl_->validated.store(backend);
    }
    catch (const config_invalid_t &) {
      impl_->validated.store({});
      return { 500, "hdr_config_invalid" };
    }
    catch (const component_untrusted_t &) {
      impl_->validated.store({});
      return { 409, "hdr_component_untrusted" };
    }
    catch (...) {
      impl_->validated.store({});
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
    if (id != NVIDIA_RTX_VIDEO_BACKEND) return { 404, "hdr_component_unknown" };
    boost::unique_lock lock(impl_->transaction, boost::try_to_lock);
    if (!lock.owns_lock()) return { 409, "hdr_save_busy" };
    maintenance_lock_t operation_lock(impl_->maintenance_file);
    if (!operation_lock) return { 409, "hdr_helper_running" };
    // 恢复不意味着强行启用。文件不匹配时保留用户设置，但禁用运行时引用，允许修复。
    impl_->validated.store({});
    try {
      const auto value = impl_->disk();
      impl_->active.store(make_immutable<settings_t>(value));
      impl_->validated.store(impl_->validate(value));
    }
    catch (...) {
      impl_->validated.store({});
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
        return catalog;
      }());
    return instance;
  }
}  // namespace hdr_enhanced
