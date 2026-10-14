/**
 * @file src/confighttp/apps.cpp
 * @brief Web UI apps handlers and routes.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "apps.h"
#include "src/config.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/file_handler.h"
#include "src/http/types.h"
#include "src/httpcommon.h"
#include "src/logging.h"
#include "src/network.h"
#include "src/nvhttp.h"
#include "src/platform/run_command.h"
#include "src/process.h"
#include "src/utility.h"
#include <atomic>
#include <boost/property_tree/json_parser.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <openssl/evp.h>
#include <set>
#include <sstream>
#include <stdexcept>

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp::apps {
  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  using namespace confighttp;

  namespace {
    // One non-blocking gate covers all mutations of the apps file.
    std::atomic<bool> apps_writing { false };

    void
    getApps(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      print_req(request);

      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      web::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(content, headers);
    }

    void
    saveApp(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;

      print_req(request);

      // Prevent concurrent write to file_apps causing file corruption
      bool expected = false;
      if (!apps_writing.compare_exchange_strong(expected, true)) {
        pt::ptree outputTree;
        outputTree.put("status", "false");
        outputTree.put("error", "Another save operation is in progress");
        std::ostringstream data;
        pt::write_json(data, outputTree);
        response->write(web::StatusCode::conflict, data.str());
        return;
      }
      auto writing_guard = util::fail_guard([]() { apps_writing = false; });

      std::stringstream ss;
      ss << request->content.rdbuf();

      pt::ptree outputTree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_json(data, outputTree);
        response->write(data.str());
      });

      pt::ptree inputTree, fileTree;

      try {
        pt::read_json(ss, inputTree);
        file_handler::read_json(config::stream.file_apps, fileTree);

        auto &apps_node = fileTree.get_child("apps"s);
        auto &input_apps_node = inputTree.get_child("apps"s);
        auto &input_edit_node = inputTree.get_child("editApp"s);

        const auto normalize_gamepad = [&](pt::ptree &app_node) {
          auto gamepad = app_node.get_optional<std::string>("gamepad");
          if (gamepad && gamepad->empty()) {
            app_node.erase("gamepad");
          }
          else if (gamepad && *gamepad != "auto"sv && *gamepad != "x360"sv && *gamepad != "ds4"sv && *gamepad != "ds5"sv) {
            outputTree.put("status", "false");
            outputTree.put("error", "Invalid per-app gamepad type");
            return false;
          }

          return true;
        };

        for (auto &[_, app_node] : input_apps_node) {
          if (!normalize_gamepad(app_node)) {
            return;
          }
        }

        // Validate app name when editing a specific app
        if (!input_edit_node.empty()) {
          auto app_name = input_edit_node.get<std::string>("name", "");
          if (app_name.empty() || app_name.size() > 256) {
            outputTree.put("status", "false");
            outputTree.put("error", "App name must be 1-256 characters");
            return;
          }
        }

        if (input_edit_node.empty()) {
          fileTree.erase("apps");
          fileTree.push_back(std::make_pair("apps", input_apps_node));
        }
        else {
          auto prep_cmd = input_edit_node.get_child_optional("prep-cmd");
          if (prep_cmd && prep_cmd->empty()) {
            input_edit_node.erase("prep-cmd");
          }

          auto detached = input_edit_node.get_child_optional("detached");
          if (detached && detached->empty()) {
            input_edit_node.erase("detached");
          }

          if (!normalize_gamepad(input_edit_node)) {
            return;
          }

          int index = input_edit_node.get<int>("index");
          input_edit_node.erase("index");

          if (index == -1) {
            apps_node.push_back(std::make_pair("", input_edit_node));
          }
          else {
            // Unfortunately Boost PT does not allow to directly edit the array, copy should do the trick
            pt::ptree newApps;
            int i = 0;
            for (auto &[_, app_node] : input_apps_node) {
              newApps.push_back(std::make_pair("", i == index ? input_edit_node : app_node));
              i++;
            }
            fileTree.erase("apps");
            fileTree.push_back(std::make_pair("apps", newApps));
          }
        }

        file_handler::write_json(config::stream.file_apps, fileTree);
      }
      catch (std::exception &e) {
        BOOST_LOG(warning) << "SaveApp: "sv << e.what();

        outputTree.put("status", "false");
        outputTree.put("error", "Invalid Input JSON");
        return;
      }

      BOOST_LOG(info) << "SaveApp: configuration saved successfully"sv;
      outputTree.put("status", "true");
      proc::refresh(config::stream.file_apps);
    }

    void
    deleteApp(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      print_req(request);

      // Prevent concurrent write to file_apps causing file corruption
      bool expected = false;
      if (!apps_writing.compare_exchange_strong(expected, true)) {
        pt::ptree outputTree;
        outputTree.put("status", "false");
        outputTree.put("error", "Another operation is in progress");
        std::ostringstream data;
        pt::write_json(data, outputTree);
        response->write(web::StatusCode::conflict, data.str());
        return;
      }
      auto writing_guard = util::fail_guard([]() { apps_writing = false; });

      pt::ptree outputTree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_json(data, outputTree);
        response->write(data.str());
      });
      pt::ptree fileTree;
      try {
        file_handler::read_json(config::stream.file_apps, fileTree);
        auto &apps_node = fileTree.get_child("apps"s);
        int index = stoi(request->path_match[1]);

        int apps_count = static_cast<int>(apps_node.size());
        if (index < 0 || index >= apps_count) {
          outputTree.put("status", "false");
          outputTree.put("error", "Invalid Index");
          return;
        }
        else {
          // Unfortunately Boost PT does not allow to directly edit the array, copy should do the trick
          pt::ptree newApps;
          int i = 0;
          for (const auto &kv : apps_node) {
            if (i++ != index) {
              newApps.push_back(std::make_pair("", kv.second));
            }
          }
          fileTree.erase("apps");
          fileTree.push_back(std::make_pair("apps", newApps));
        }
        file_handler::write_json(config::stream.file_apps, fileTree);
      }
      catch (std::exception &e) {
        BOOST_LOG(warning) << "DeleteApp: "sv << e.what();
        outputTree.put("status", "false");
        outputTree.put("error", "Invalid File JSON");
        return;
      }

      BOOST_LOG(info) << "DeleteApp: configuration deleted successfully"sv;
      outputTree.put("status", "true");
      proc::refresh(config::stream.file_apps);
    }

    /**
     * @brief Delete multiple apps in a single atomic operation.
     *
     * Expects JSON body: {"indices":[<int>, <int>, ...]}
     *
     * Rationale: clients that want to delete N apps cannot just loop on
     * DELETE /api/apps/{id} — each successful delete shifts the remaining
     * indices, and a second concurrent caller is rejected by apps_writing.
     * By taking the lock once and rebuilding the array under it, we avoid both
     * problems: indices are interpreted against a single snapshot, the JSON file
     * is rewritten once, and proc::refresh runs only once.
     *
     * Response:
     *   200 {"status":"true",  "deleted":<N>, "remaining":<M>}
     *   400 {"status":"false", "error":"..."} for content-type / JSON / index /
     *        file-write business errors. Follows uploadCover() convention in
     *        this same file (status code derived from presence of 'error' key
     *        inside the fail_guard).
     *   401  not authenticated (via authenticate)
     *   409  another apps-writer is already in flight
     *
     * @api_examples{/api/apps/batch-delete| POST| {"indices":[2,5,7]}}
     */
    void
    batchDeleteApps(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;

      print_req(request);

      // Same single-writer guard as deleteApp / saveApp.
      bool expected = false;
      if (!apps_writing.compare_exchange_strong(expected, true)) {
        pt::ptree busy;
        busy.put("status", "false");
        busy.put("error", "Another operation is in progress");
        std::ostringstream data;
        pt::write_json(data, busy);
        response->write(web::StatusCode::conflict, data.str());
        return;
      }
      auto writing_guard = util::fail_guard([]() { apps_writing = false; });

      pt::ptree outputTree;
      auto g = util::fail_guard([&]() {
        // Mirror uploadCover's convention in this file: when fail_guard fires
        // with an 'error' field, emit 4xx so clients can distinguish business
        // failures from success rather than parsing status:"false" out of a 200.
        web::StatusCode code = web::StatusCode::ok;
        if (outputTree.get_child_optional("error").has_value()) {
          code = web::StatusCode::bad_request;
        }
        std::ostringstream data;
        pt::write_json(data, outputTree);
        response->write(code, data.str());
      });

      std::stringstream ss;
      ss << request->content.rdbuf();

      std::set<int> indices_to_remove;
      try {
        pt::ptree body;
        pt::read_json(ss, body);

        auto indices_node = body.get_child_optional("indices");
        if (!indices_node) {
          outputTree.put("status", "false");
          outputTree.put("error", "Missing 'indices' array");
          return;
        }

        if (indices_node->size() > 1024) {
          outputTree.put("status", "false");
          outputTree.put("error", "Too many indices in a single request");
          return;
        }
        for (const auto &kv : *indices_node) {
          // boost::property_tree json reader stores array values as anonymous
          // children with empty keys. get_value<int>() will throw if the value
          // is not parseable as an integer.
          int idx = kv.second.get_value<int>();
          indices_to_remove.insert(idx);
        }
      }
      catch (std::exception &e) {
        BOOST_LOG(warning) << "BatchDeleteApps: invalid JSON body: "sv << e.what();
        outputTree.put("status", "false");
        outputTree.put("error", "Invalid JSON body");
        return;
      }

      pt::ptree fileTree;
      try {
        file_handler::read_json(config::stream.file_apps, fileTree);
      }
      catch (std::exception &e) {
        BOOST_LOG(warning) << "BatchDeleteApps: "sv << e.what();
        outputTree.put("status", "false");
        outputTree.put("error", "Invalid File JSON");
        return;
      }

      // Validate every index against the single snapshot we just loaded BEFORE
      // any write, so a partially invalid request fails atomically.
      const int apps_count = static_cast<int>(fileTree.get_child("apps"s).size());
      for (int idx : indices_to_remove) {
        if (idx < 0 || idx >= apps_count) {
          outputTree.put("status", "false");
          outputTree.put("error", "Invalid Index");
          return;
        }
      }

      // Empty selection: success no-op. Skip the write+refresh, but still emit
      // 'remaining' so the success contract matches the non-empty path.
      if (indices_to_remove.empty()) {
        outputTree.put("status", "true");
        outputTree.put("deleted", 0);
        outputTree.put("remaining", apps_count);
        return;
      }

      int deleted_count = 0;
      int remaining_count = 0;
      try {
        auto &apps_node = fileTree.get_child("apps"s);
        pt::ptree newApps;
        int i = 0;
        for (const auto &kv : apps_node) {
          if (indices_to_remove.find(i) == indices_to_remove.end()) {
            newApps.push_back(std::make_pair("", kv.second));
          }
          else {
            ++deleted_count;
          }
          ++i;
        }
        remaining_count = static_cast<int>(newApps.size());
        fileTree.erase("apps");
        fileTree.push_back(std::make_pair("apps", newApps));

        file_handler::write_json(config::stream.file_apps, fileTree);
      }
      catch (std::exception &e) {
        BOOST_LOG(warning) << "BatchDeleteApps: "sv << e.what();
        outputTree.put("status", "false");
        outputTree.put("error", "Invalid File JSON");
        return;
      }

      BOOST_LOG(info) << "BatchDeleteApps: removed "sv << deleted_count
                      << " app(s), "sv << remaining_count << " remaining"sv;
      outputTree.put("status", "true");
      outputTree.put("deleted", deleted_count);
      outputTree.put("remaining", remaining_count);
      proc::refresh(config::stream.file_apps);
    }

    void
    uploadCover(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;

      std::stringstream ss;
      std::stringstream configStream;
      ss << request->content.rdbuf();
      pt::ptree outputTree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        web::StatusCode code = web::StatusCode::ok;
        if (outputTree.get_child_optional("error").has_value()) {
          code = web::StatusCode::bad_request;
        }

        pt::write_json(data, outputTree);
        response->write(code, data.str());
      });
      pt::ptree inputTree;
      try {
        pt::read_json(ss, inputTree);
      }
      catch (std::exception &e) {
        BOOST_LOG(warning) << "UploadCover: "sv << e.what();
        outputTree.put("status", "false");
        outputTree.put("error", e.what());
        return;
      }

      auto key = inputTree.get("key", "");
      if (key.empty()) {
        outputTree.put("error", "Cover key is required");
        return;
      }
      auto url = inputTree.get("url", "");

      const std::string coverdir = file_handler::path_to_utf8(platf::appdata() / "covers");
      file_handler::make_directory(coverdir);

      const auto cover_path = file_handler::path_from_utf8(coverdir) / (http::url_escape(key) + ".png");
      const std::string path = file_handler::path_to_utf8(cover_path);
      if (!url.empty()) {
        if (!http::download_public_cover_image(url, path)) {
          outputTree.put("error", "Failed to download public HTTPS cover");
          return;
        }
      }
      else {
        // Limit base64 data size to prevent memory exhaustion
        // (10MB decoded to about 7.5MB, enough for cover images)
        constexpr std::size_t MAX_COVER_BASE64_SIZE = 10 * 1024 * 1024;
        auto base64_str = inputTree.get<std::string>("data");
        if (base64_str.size() > MAX_COVER_BASE64_SIZE) {
          outputTree.put("error", "Cover image too large (max 10MB)");
          return;
        }
        std::string data;
        try {
          data = web::decode_base64(base64_str);
        }
        catch (const std::invalid_argument &e) {
          outputTree.put("error", e.what());
          return;
        }

        std::ofstream imgfile(file_handler::path_from_utf8(path), std::ios::binary);
        if (!imgfile.is_open()) {
          outputTree.put("error", "Failed to create file");
          return;
        }
        imgfile.write(data.data(), (int) data.size());
        imgfile.close();

        if (imgfile.fail()) {
          outputTree.put("error", "Failed to write file");
          return;
        }
      }
      outputTree.put("path", path);
    }

    void
    closeApp(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      print_req(request);

      pt::ptree outputTree;

      auto g = util::fail_guard([&]() {
        std::ostringstream data;
        pt::write_json(data, outputTree);
        response->write(data.str());
      });

      proc::proc.terminate();
      outputTree.put("status", true);
    }

    /**
     * @brief 计算文件的SHA256哈希值
     * @param filepath 文件路径
     * @return SHA256哈希字符串，如果失败返回空字符串
     */
    std::string
    calculate_file_hash(const std::string &filepath) {
      const auto native_path = file_handler::path_from_utf8(filepath);
      if (filepath.empty() || !fs::exists(native_path)) {
        return "";
      }

      std::ifstream file(native_path, std::ios::binary);
      if (!file.is_open()) {
        return "";
      }

      EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
      if (!mdctx) {
        return "";
      }

      if (EVP_DigestInit_ex(mdctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(mdctx);
        return "";
      }

      char buf[1024 * 16];
      while (file.good()) {
        file.read(buf, sizeof(buf));
        if (file.gcount() > 0) {
          if (EVP_DigestUpdate(mdctx, buf, file.gcount()) != 1) {
            EVP_MD_CTX_free(mdctx);
            file.close();
            return "";
          }
        }
      }
      file.close();

      unsigned char hash[SHA256_DIGEST_LENGTH];
      unsigned int hash_len = 0;
      if (EVP_DigestFinal_ex(mdctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(mdctx);
        return "";
      }
      EVP_MD_CTX_free(mdctx);

      std::stringstream ss;
      ss << std::hex << std::setfill('0');
      for (unsigned int i = 0; i < hash_len; i++) {
        ss << std::setw(2) << static_cast<int>(hash[i]);
      }
      return ss.str();
    }

    /**
     * @brief 从命令字符串中提取可执行文件路径
     * @param cmd 完整命令字符串
     * @return 可执行文件路径
     */
    std::string
    extract_executable_path(const std::string &cmd) {
      if (cmd.empty()) {
        return "";
      }

      std::string trimmed = cmd;
      // 移除前导空格
      size_t start = trimmed.find_first_not_of(" \t");
      if (start != std::string::npos) {
        trimmed = trimmed.substr(start);
      }

      // 处理引号包裹的路径
      if (!trimmed.empty() && trimmed[0] == '"') {
        size_t end = trimmed.find('"', 1);
        if (end != std::string::npos) {
          return trimmed.substr(1, end - 1);
        }
      }

      // 提取第一个空格前的部分（可执行文件路径）
      size_t space = trimmed.find(' ');
      if (space != std::string::npos) {
        return trimmed.substr(0, space);
      }

      return trimmed;
    }

    void
    testMenuCmd(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;

      constexpr bool test_command_enabled = false;
      if (!test_command_enabled) {
        const nlohmann::json body {
          { "status", false },
          { "error", "Test command execution is temporarily disabled" },
          { "error_code", "TEST_COMMAND_DISABLED" },
        };
        web::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(web::StatusCode::service_unavailable, body.dump(), headers);
        return;
      }

      // 安全限制：只允许局域网访问测试命令功能
      auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
      auto ip_type = net::from_address(address);

      if (ip_type != net::PC) {
        BOOST_LOG(warning) << "TestMenuCmd: Access denied from non-local network: " << address;
        pt::ptree outputTree;
        outputTree.put("status", false);
        outputTree.put("error", "Test command feature is only available from local network");

        std::ostringstream data;
        pt::write_json(data, outputTree);
        response->write(data.str());
        return;
      }

      print_req(request);

      std::stringstream ss;
      ss << request->content.rdbuf();

      pt::ptree inputTree, outputTree;

      auto g = util::fail_guard([&]() {
        std::ostringstream data;
        pt::write_json(data, outputTree);
        response->write(data.str());
      });

      try {
        pt::read_json(ss, inputTree);
        auto cmd = inputTree.get<std::string>("cmd");
        auto working_dir = inputTree.get<std::string>("working_dir", "");
        auto elevated = inputTree.get<bool>("elevated", false);

        // 安全检查：命令不能为空
        if (cmd.empty()) {
          BOOST_LOG(warning) << "TestMenuCmd: Empty command provided";
          outputTree.put("status", false);
          outputTree.put("error", "Command cannot be empty");
          return;
        }

        // 安全检查：命令长度限制（防止过长的命令）
        if (cmd.length() > 4096) {
          BOOST_LOG(warning) << "TestMenuCmd: Command too long (" << cmd.length() << " characters)";
          outputTree.put("status", false);
          outputTree.put("error", "Command exceeds maximum length");
          return;
        }

        // 提取可执行文件路径并计算SHA256哈希值
        std::string executable_path = extract_executable_path(cmd);
        std::string file_hash;

        if (!executable_path.empty()) {
          // 如果是相对路径，尝试解析为绝对路径
          auto exec_path = file_handler::path_from_utf8(executable_path);
          if (!exec_path.is_absolute()) {
            // 在PATH中查找或使用工作目录
            if (!working_dir.empty()) {
              exec_path = file_handler::path_from_utf8(working_dir) / exec_path;
            }
          }

          file_hash = calculate_file_hash(file_handler::path_to_utf8(exec_path));

          if (file_hash.empty() && fs::exists(exec_path)) {
            BOOST_LOG(warning) << "TestMenuCmd: Failed to calculate hash for executable: " << file_handler::path_to_utf8(exec_path);
          }
        }

        // 记录详细信息用于审计（包含文件哈希值）
        BOOST_LOG(info) << "Testing menu command from " << address << ": [" << cmd << "]";
        if (!file_hash.empty()) {
          BOOST_LOG(info) << "Executable SHA256: " << file_hash << " (" << executable_path << ")";
        }
        else if (!executable_path.empty()) {
          BOOST_LOG(warning) << "Could not verify executable: " << executable_path;
        }

        std::error_code ec;
        boost::filesystem::path work_dir;

        if (!working_dir.empty()) {
          const auto native_working_dir = file_handler::path_from_utf8(working_dir);
          // 验证工作目录是否存在
          if (!fs::exists(native_working_dir) || !fs::is_directory(native_working_dir)) {
            BOOST_LOG(warning) << "TestMenuCmd: Invalid working directory: " << working_dir;
            outputTree.put("status", false);
            outputTree.put("error", "Invalid working directory");
            return;
          }
#ifdef _WIN32
          work_dir = boost::filesystem::path(native_working_dir.wstring());
#else
          work_dir = boost::filesystem::path(native_working_dir.string());
#endif
        }
        else {
          work_dir = boost::filesystem::current_path();
        }

        // 执行命令
        auto child = platf::run_command(elevated, true, cmd, work_dir, proc::proc.get_env(), nullptr, ec, nullptr);

        if (ec) {
          BOOST_LOG(warning) << "Failed to run menu command [" << cmd << "]: " << ec.message();
          outputTree.put("status", false);
          outputTree.put("error", "Failed to execute command: " + ec.message());
        }
        else {
          BOOST_LOG(info) << "Successfully executed menu command [" << cmd << "]";
          child.detach();
          outputTree.put("status", true);
          outputTree.put("message", "Command executed successfully");
        }
      }
      catch (std::exception &e) {
        BOOST_LOG(warning) << "TestMenuCmd error: " << e.what();
        outputTree.put("status", false);
        outputTree.put("error", e.what());
        return;
      }
    }

  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/api/apps$"]["GET"] = getApps;
    server.resource["^/api/apps$"]["POST"] = saveApp;
    server.resource["^/api/apps/([0-9]+)$"]["DELETE"] = deleteApp;
    server.resource["^/api/apps/batch-delete$"]["POST"] = batchDeleteApps;
    server.resource["^/api/apps/close$"]["POST"] = closeApp;
    server.resource["^/api/covers/upload$"]["POST"] = uploadCover;
    server.resource["^/api/apps/test-menu-cmd$"]["POST"] = testMenuCmd;
  }

}  // namespace confighttp::apps
