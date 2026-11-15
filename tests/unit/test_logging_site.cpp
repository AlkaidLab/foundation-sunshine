/**
 * @file tests/unit/test_logging_site.cpp
 * @brief Error and fatal log lines carry file:line:function of the statement; other severities are unchanged.
 */
#include "src/logging_severity.h"

#include <gtest/gtest.h>

#include <boost/core/null_deleter.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>

#include <functional>
#include <regex>
#include <sstream>

// The loggers are defined in logging.cpp, which needs the whole host; the test owns a set with the same names and severities.
boost::log::sources::severity_logger<int> verbose(0);
boost::log::sources::severity_logger<int> debug(1);
boost::log::sources::severity_logger<int> info(2);
boost::log::sources::severity_logger<int> warning(3);
boost::log::sources::severity_logger<int> error(4);
boost::log::sources::severity_logger<int> fatal(5);

namespace {
  namespace bl = boost::log;

  // consteval: the compiler cuts these, nothing is computed at run time
  static_assert(std::string_view { logging::trim_source_path("/home/dev/build/src/stream.cpp") } == "src/stream.cpp");
  static_assert(std::string_view { logging::trim_source_path("/home/dev/src/tools/src_assets/a.cpp") } == "src/tools/src_assets/a.cpp");
  static_assert(std::string_view { logging::trim_source_path("C:\\work\\tools\\installer.cpp") } == "tools\\installer.cpp");
  static_assert(std::string_view { logging::trim_source_path("/home/dev/mysrc/x.cpp") } == "x.cpp");  // "mysrc" is not the directory src
  static_assert(std::string_view { logging::trim_source_path("/home/dev/build/gen.cpp") } == "gen.cpp");
  static_assert(std::string_view { logging::trim_source_path("tests/unit/t.cpp") } == "tests/unit/t.cpp");

  /** A sink that writes what the host's formatter writes after the level: the message, then the site. */
  struct capture_t {
    using sink_t = bl::sinks::synchronous_sink<bl::sinks::text_ostream_backend>;
    std::ostringstream out;
    boost::shared_ptr<sink_t> sink;

    capture_t() {
      sink = boost::make_shared<sink_t>();
      sink->locked_backend()->add_stream(boost::shared_ptr<std::ostream>(&out, boost::null_deleter()));
      sink->locked_backend()->auto_flush(true);
      sink->set_formatter([](const bl::record_view &view, bl::formatting_ostream &os) {
        const auto message = view.attribute_values()["Message"].extract<std::string>();
        os << (message ? message.get() : std::string {});
        logging::append_site(os, view);
      });
      bl::core::get()->add_sink(sink);
    }

    ~capture_t() {
      bl::core::get()->remove_sink(sink);
    }
  };
}  // namespace

TEST(LoggingSite, AnErrorLineEndsWithFileLineAndFunction) {
  capture_t log;
  const int line = __LINE__ + 1;
  BOOST_LOG(error) << "encoder init failed " << 7;
  EXPECT_EQ(log.out.str(), "encoder init failed 7 [tests/unit/test_logging_site.cpp:" + std::to_string(line) + ":TestBody]\n");
}

TEST(LoggingSite, AFatalLineHasASiteToo) {
  capture_t log;
  BOOST_LOG(fatal) << "cannot continue";
  EXPECT_TRUE(std::regex_match(log.out.str(), std::regex(R"(cannot continue \[tests/unit/test_logging_site\.cpp:\d+:TestBody\]\n)"))) << log.out.str();
}

TEST(LoggingSite, LinesBelowErrorAreUnchanged) {
  capture_t log;
  BOOST_LOG(verbose) << "a";
  BOOST_LOG(debug) << "b";
  BOOST_LOG(info) << "c";
  BOOST_LOG(warning) << "d";
  EXPECT_EQ(log.out.str(), "a\nb\nc\nd\n");
}

TEST(LoggingSite, TheSiteIsAFileOfTheSourceTreeNeverABuildOrHomeDirectory) {
  capture_t log;
  BOOST_LOG(error) << "x";
  const auto text = log.out.str();
  EXPECT_EQ(text.find("/home"), std::string::npos) << text;
  EXPECT_EQ(text.find("/var"), std::string::npos) << text;
  EXPECT_NE(text.find("[tests/"), std::string::npos) << text;
}

TEST(LoggingSite, TheMacroStillWorksAsTheBodyOfAnUnbracedIfElse) {
  capture_t log;
  const bool yes = true;
  if (yes)
    BOOST_LOG(error) << "then";
  else
    BOOST_LOG(error) << "else";
  EXPECT_EQ(log.out.str().rfind("then [tests/", 0), 0u) << log.out.str();
  EXPECT_EQ(log.out.str().find("else"), std::string::npos);
}

TEST(LoggingSite, ALoggerReachedThroughAReferenceKeepsItsSeverity) {
  capture_t log;
  auto &ref = error;
  BOOST_LOG(ref) << "via reference";
  auto wrapped = std::ref(warning);
  BOOST_LOG(wrapped.get()) << "via wrapper";
  EXPECT_NE(log.out.str().find("via reference [tests/"), std::string::npos) << log.out.str();
  EXPECT_NE(log.out.str().find("via wrapper\n"), std::string::npos) << log.out.str();
}
