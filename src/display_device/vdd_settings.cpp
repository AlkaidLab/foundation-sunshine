/**
 * @file src/display_device/vdd_settings.cpp
 * @brief Virtual display settings persistence.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "vdd_settings.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include <boost/algorithm/string.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/regex.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace std::literals;

namespace display_device::vdd_settings {
  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  namespace {
    std::vector<std::string>
    split(const std::string &str, char delimiter) {
      std::vector<std::string> tokens;
      size_t start = 0, end = 0;
      while ((end = str.find(delimiter, start)) != std::string::npos) {
        tokens.push_back(str.substr(start, end - start));
        start = end + 1;
      }
      tokens.push_back(str.substr(start));
      return tokens;
    }

  }  // namespace

  bool
  save(std::string resArray, std::string fpsArray, std::string gpu_name) {
    pt::ptree iddOptionTree;
    pt::ptree global_node;
    pt::ptree resolutions_nodes;

    // prepare resolutions setting for vdd
    boost::regex pattern("\\[|\\]|\\s+");
    char delimiter = ',';

    // 添加全局刷新率到global节点
    for (const auto &fps : split(boost::regex_replace(fpsArray, pattern, ""), delimiter)) {
      global_node.add("g_refresh_rate", fps);
    }

    std::string str = boost::regex_replace(resArray, pattern, "");
    boost::algorithm::trim(str);
    for (const auto &resolution : split(str, delimiter)) {
      auto index = resolution.find('x');
      if (index == std::string::npos) {
        return false;
      }
      pt::ptree res_node;
      res_node.put("width", resolution.substr(0, index));
      res_node.put("height", resolution.substr(index + 1));
      resolutions_nodes.push_back(std::make_pair("resolution"s, res_node));
    }

    // 类似于 config.cpp 中的 path_f 函数逻辑，使用相对路径
    std::filesystem::path idd_option_path = platf::appdata() / "vdd_settings.xml";

    BOOST_LOG(info) << "VDD配置文件路径: " << file_handler::path_to_utf8(idd_option_path);

    if (!fs::exists(idd_option_path)) {
      return false;
    }

    // 先读取现有配置文件
    pt::ptree existing_root;
    pt::ptree root;

    try {
      {
        std::ifstream input(idd_option_path, std::ios::binary);
        if (!input.is_open()) {
          throw std::runtime_error("Unable to open VDD configuration file");
        }
        pt::read_xml(input, existing_root);
      }
      // 如果现有配置文件中已有vdd_settings节点
      if (existing_root.get_child_optional("vdd_settings")) {
        // 复制现有配置
        iddOptionTree = existing_root.get_child("vdd_settings");

        // 更新需要更改的部分
        pt::ptree monitor_node;
        monitor_node.put("count", 1);

        pt::ptree gpu_node;
        gpu_node.put("friendlyname", gpu_name.empty() ? "default" : gpu_name);

        // 替换配置
        iddOptionTree.put_child("monitors", monitor_node);
        iddOptionTree.put_child("gpu", gpu_node);
        iddOptionTree.put_child("global", global_node);
        iddOptionTree.put_child("resolutions", resolutions_nodes);
      }
      else {
        // 如果没有vdd_settings节点，创建新的
        pt::ptree monitor_node;
        monitor_node.put("count", 1);

        pt::ptree gpu_node;
        gpu_node.put("friendlyname", gpu_name.empty() ? "default" : gpu_name);

        iddOptionTree.add_child("monitors", monitor_node);
        iddOptionTree.add_child("gpu", gpu_node);
        iddOptionTree.add_child("global", global_node);
        iddOptionTree.add_child("resolutions", resolutions_nodes);
      }
    }
    catch (std::exception &e) {
      // 读取失败，创建新的配置
      BOOST_LOG(warning) << "读取现有VDD配置失败，创建新配置: " << e.what();

      pt::ptree monitor_node;
      monitor_node.put("count", 1);

      pt::ptree gpu_node;
      gpu_node.put("friendlyname", gpu_name.empty() ? "default" : gpu_name);

      iddOptionTree.add_child("monitors", monitor_node);
      iddOptionTree.add_child("gpu", gpu_node);
      iddOptionTree.add_child("global", global_node);
      iddOptionTree.add_child("resolutions", resolutions_nodes);
    }

    root.add_child("vdd_settings", iddOptionTree);
    try {
      // 使用更紧凑的XML格式设置，减少不必要的空白
      auto setting = boost::property_tree::xml_writer_make_settings<std::string>(' ', 2);
      std::ostringstream oss;
      write_xml(oss, root, setting);

      // 清理多余空行，保持XML格式整洁
      std::string xml_content = oss.str();
      boost::regex empty_lines_regex("\\n\\s*\\n");
      xml_content = boost::regex_replace(xml_content, empty_lines_regex, "\n");

      std::ofstream file(idd_option_path);
      if (!file.is_open()) {
        throw std::runtime_error("Unable to open VDD configuration file");
      }
      file << xml_content;
      file.flush();
      if (!file) {
        throw std::runtime_error("Unable to write VDD configuration file");
      }

      return true;
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "写入VDD配置失败: " << e.what();
      return false;
    }
  }

}  // namespace display_device::vdd_settings
