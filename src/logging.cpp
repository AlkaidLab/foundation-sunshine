/**
 * @file src/logging.cpp
 * @brief Definitions for logging related functions.
 */
// standard includes
#include <fstream>
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <ctime>

// lib includes
#include <boost/core/null_deleter.hpp>
#include <boost/format.hpp>
#include <boost/log/attributes/clock.hpp>
#include <boost/log/common.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/utility/exception_handler.hpp>
#include <boost/log/utility/setup/file.hpp>

// local includes
#include "logging.h"

extern "C" {
#include <libavutil/log.h>
}

using namespace std::literals;

namespace bl = boost::log;

boost::shared_ptr<boost::log::sinks::asynchronous_sink<boost::log::sinks::text_ostream_backend>> console_sink;
boost::shared_ptr<boost::log::sinks::synchronous_sink<boost::log::sinks::text_file_backend>> file_sink_ptr;
boost::shared_ptr<boost::log::sinks::asynchronous_sink<boost::log::sinks::text_ostream_backend>> file_ostream_sink;

bl::sources::severity_logger<int> verbose(0);  // Dominating output
bl::sources::severity_logger<int> debug(1);  // Follow what is happening
bl::sources::severity_logger<int> info(2);  // Should be informed about
bl::sources::severity_logger<int> warning(3);  // Strange events
bl::sources::severity_logger<int> error(4);  // Recoverable errors
bl::sources::severity_logger<int> fatal(5);  // Unrecoverable errors
#ifdef SUNSHINE_TESTS
bl::sources::severity_logger<int> tests(10);  // Automatic tests output
#endif

BOOST_LOG_ATTRIBUTE_KEYWORD(severity, "Severity", int)

namespace logging {
  deinit_t::~deinit_t() {
    deinit();
  }

  void
  deinit() {
    log_flush();
    if (console_sink) {
      bl::core::get()->remove_sink(console_sink);
      console_sink.reset();
    }
    if (file_sink_ptr) {
      bl::core::get()->remove_sink(file_sink_ptr);
      file_sink_ptr.reset();
    }
    if (file_ostream_sink) {
      bl::core::get()->remove_sink(file_ostream_sink);
      file_ostream_sink.reset();
    }
  }

  /**
   * @brief 将现有日志文件转写到带日期的备份文件中
   * @param log_file 当前日志文件路径
   */
  void
  archive_existing_log(const std::string &log_file) {
    namespace fs = std::filesystem;
    
    // 检查日志文件是否存在
    if (!fs::exists(log_file)) {
      return;
    }
    
    try {
      // 获取当前时间
      auto now = std::chrono::system_clock::now();
      auto time_t = std::chrono::system_clock::to_time_t(now);
      auto tm = *std::localtime(&time_t);
      
      // 生成带日期的备份文件名（只精确到日期）
      std::ostringstream backup_name;
      backup_name << "sunshine_" 
                  << std::put_time(&tm, "%Y%m%d") 
                  << ".log";
      
      // 构建备份文件路径
      fs::path log_path(log_file);
      fs::path backup_path = log_path.parent_path() / backup_name.str();
      
      // 如果备份文件已存在，则追加到文件尾部
      if (fs::exists(backup_path)) {
        std::ifstream source(log_file, std::ios::binary);
        std::ofstream dest(backup_path, std::ios::binary | std::ios::app);
        
        if (source && dest) {
          dest << source.rdbuf();
          dest.close();
          source.close();
          
          // 删除原日志文件
          fs::remove(log_file);
          
          BOOST_LOG(info) << "Appended log file to: " << backup_path.string();
        }
        else {
          BOOST_LOG(warning) << "Failed to open files for append operation";
        }
      }
      else {
        // 备份文件不存在，直接重命名
        fs::rename(log_file, backup_path);
        BOOST_LOG(info) << "Archived log file to: " << backup_path.string();
      }
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "Failed to archive log file: " << e.what();
    }
  }

  void
  formatter(const boost::log::record_view &view, boost::log::formatting_ostream &os) {
    constexpr const char *message = "Message";
    constexpr const char *severity = "Severity";

    auto log_level = view.attribute_values()[severity].extract<int>().get();

    std::string_view log_type;
    switch (log_level) {
      case 0:
        log_type = "Verbose: "sv;
        break;
      case 1:
        log_type = "Debug: "sv;
        break;
      case 2:
        log_type = "Info: "sv;
        break;
      case 3:
        log_type = "Warning: "sv;
        break;
      case 4:
        log_type = "Error: "sv;
        break;
      case 5:
        log_type = "Fatal: "sv;
        break;
#ifdef SUNSHINE_TESTS
      case 10:
        log_type = "Tests: "sv;
        break;
#endif
    };

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - std::chrono::time_point_cast<std::chrono::seconds>(now));

    auto t = std::chrono::system_clock::to_time_t(now);
    auto lt = *std::localtime(&t);

    os << "["sv << std::put_time(&lt, "%Y-%m-%d %H:%M:%S.") << boost::format("%03u") % ms.count() << "]: "sv
       << log_type << view.attribute_values()[message].extract<std::string>();
  }

  /**
   * @brief Initialize the logging system.
   * @param min_log_level The minimum log level to output.
   * @param log_file The log file to write to.
   * @param restore_log Whether to restore existing log file (true=restore, false=overwrite).
   * @return An object that will deinitialize the logging system when it goes out of scope.
   * @examples
   * log_init(2, "sunshine.log", true);
   * @examples_end
   */
  [[nodiscard]] std::unique_ptr<deinit_t>
  init(int min_log_level, const std::string &log_file, bool restore_log) {
    if (console_sink || file_sink_ptr || file_ostream_sink) {
      // Deinitialize the logging system before reinitializing it. This can probably only ever be hit in tests.
      deinit();
    }

    setup_av_logging(min_log_level);

    // Console sink (async, stdout)
#ifndef SUNSHINE_TESTS
    console_sink = boost::make_shared<text_sink>();
    boost::shared_ptr<std::ostream> stream { &std::cout, boost::null_deleter() };
    console_sink->locked_backend()->add_stream(stream);
    console_sink->locked_backend()->auto_flush(true);
    console_sink->set_filter(severity >= min_log_level);
    console_sink->set_formatter(&formatter);
    console_sink->set_exception_handler(bl::make_exception_suppressor());
    bl::core::get()->add_sink(console_sink);
#endif

    // 转写现有日志文件
    if (config::sunshine.restore_log) {
      archive_existing_log(log_file);
    }

    // File sink with rotation support
    namespace fs = std::filesystem;
    const auto max_log_size_mb = config::sunshine.max_log_size_mb;
    const auto log_dir = fs::path(log_file).parent_path();
    const auto log_filename = fs::path(log_file).filename().string();

    if (max_log_size_mb > 0) {
      // When restore_log is false, truncate the existing log file before starting rotation
      if (!config::sunshine.restore_log && fs::exists(log_file)) {
        std::error_code ec;
        fs::remove(log_file, ec);
      }

      // Use text_file_backend with automatic size-based rotation
      auto file_backend = boost::make_shared<bl::sinks::text_file_backend>(
        bl::keywords::file_name = log_file,
        bl::keywords::rotation_size = static_cast<uintmax_t>(max_log_size_mb) * 1024 * 1024,
        bl::keywords::open_mode = std::ios_base::out | std::ios_base::app
      );

      // Set up file collector to manage rotated log files
      file_backend->set_file_collector(bl::sinks::file::make_collector(
        bl::keywords::target = (log_dir / "logs_archive").string(),
        bl::keywords::max_size = static_cast<uintmax_t>(max_log_size_mb) * 1024 * 1024 * 5,  // Keep up to 5x max_size total
        bl::keywords::max_files = 10
      ));

      file_backend->auto_flush(true);
      // Scan for any previously rotated files to properly manage the archive
      file_backend->scan_for_files();

      file_sink_ptr = boost::make_shared<file_sink>(file_backend);
      file_sink_ptr->set_filter(severity >= min_log_level);
      file_sink_ptr->set_formatter(&formatter);
      file_sink_ptr->set_exception_handler(bl::make_exception_suppressor());
      bl::core::get()->add_sink(file_sink_ptr);
    }
    else {
      // No rotation: use simple text_ostream_backend (original behavior)
      file_ostream_sink = boost::make_shared<text_sink>();
      file_ostream_sink->locked_backend()->add_stream(boost::make_shared<std::ofstream>(log_file, std::ios_base::out));
      file_ostream_sink->locked_backend()->auto_flush(true);
      file_ostream_sink->set_filter(severity >= min_log_level);
      file_ostream_sink->set_formatter(&formatter);
      file_ostream_sink->set_exception_handler(bl::make_exception_suppressor());
      bl::core::get()->add_sink(file_ostream_sink);
    }

    return std::make_unique<deinit_t>();
  }

  void
  setup_av_logging(int min_log_level) {
    if (min_log_level >= 1) {
      av_log_set_level(AV_LOG_QUIET);
    }
    else {
      av_log_set_level(AV_LOG_DEBUG);
    }
    av_log_set_callback([](void *ptr, int level, const char *fmt, va_list vl) {
      static int print_prefix = 1;
      char buffer[1024];

      av_log_format_line(ptr, level, fmt, vl, buffer, sizeof(buffer), &print_prefix);
      if (level <= AV_LOG_ERROR) {
        // We print AV_LOG_FATAL at the error level. FFmpeg prints things as fatal that
        // are expected in some cases, such as lack of codec support or similar things.
        BOOST_LOG(error) << buffer;
      }
      else if (level <= AV_LOG_WARNING) {
        BOOST_LOG(warning) << buffer;
      }
      else if (level <= AV_LOG_INFO) {
        BOOST_LOG(info) << buffer;
      }
      else if (level <= AV_LOG_VERBOSE) {
        // AV_LOG_VERBOSE is less verbose than AV_LOG_DEBUG
        BOOST_LOG(debug) << buffer;
      }
      else {
        BOOST_LOG(verbose) << buffer;
      }
    });
  }

  void
  log_flush() {
    if (console_sink) {
      console_sink->flush();
    }
    if (file_sink_ptr) {
      file_sink_ptr->flush();
    }
    if (file_ostream_sink) {
      file_ostream_sink->flush();
    }
  }

  void
  print_help(const char *name) {
    std::cout
      << "Usage: "sv << name << " [options] [/path/to/configuration_file] [--cmd]"sv << std::endl
      << "    Any configurable option can be overwritten with: \"name=value\""sv << std::endl
      << std::endl
      << "    Note: The configuration will be created if it doesn't exist."sv << std::endl
      << std::endl
      << "    --help                    | print help"sv << std::endl
      << "    --creds username password | set user credentials for the Web manager"sv << std::endl
      << "    --version                 | print the version of sunshine"sv << std::endl
      << std::endl
      << "    flags"sv << std::endl
      << "        -0 | Read PIN from stdin"sv << std::endl
      << "        -1 | Do not load previously saved state and do retain any state after shutdown"sv << std::endl
      << "           | Effectively starting as if for the first time without overwriting any pairings with your devices"sv << std::endl
      << "        -2 | Force replacement of headers in video stream"sv << std::endl
      << "        -p | Enable/Disable UPnP"sv << std::endl
      << std::endl;
  }

  std::string
  bracket(const std::string &input) {
    return "["s + input + "]"s;
  }

  std::wstring
  bracket(const std::wstring &input) {
    return L"["s + input + L"]"s;
  }

}  // namespace logging
