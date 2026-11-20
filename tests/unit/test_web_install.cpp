/**
 * @file tests/unit/test_web_install.cpp
 * @brief The Web UI folder check, the restore of a folder an update left aside, and the notice.
 */
#include "src/confighttp/web_install.h"

#include <fstream>
#include <gtest/gtest.h>

namespace {
  namespace fs = std::filesystem;
  using namespace confighttp::web_install;

  class WebInstall: public testing::Test {
  protected:
    fs::path root;

    void
    SetUp() override {
      root = fs::temp_directory_path() / ("web-install-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" + testing::UnitTest::GetInstance()->current_test_info()->name());
      fs::remove_all(root);
      fs::create_directories(root);
    }

    void
    TearDown() override {
      std::error_code error;
      fs::remove_all(root, error);
    }

    static void
    write(const fs::path &file, const std::string &content = "x") {
      fs::create_directories(file.parent_path());
      std::ofstream(file, std::ios::binary) << content;
    }

    void
    make_complete(const fs::path &dir, const std::string &tag) {
      write(dir / "index.html", tag);
      write(dir / "login.html", tag);
      write(dir / "assets" / "app.js", tag);
    }

    // a folder the new installer finished
    void
    make_marked(const fs::path &dir, const std::string &tag) {
      make_complete(dir, tag);
      write(dir / std::string(complete_marker), "complete");
    }
  };
}  // namespace

TEST_F(WebInstall, AFolderNeedsBothPagesAndAScript) {
  EXPECT_FALSE(is_complete(root / "web"));
  write(root / "web" / "index.html");
  EXPECT_FALSE(is_complete(root / "web"));
  write(root / "web" / "login.html");
  EXPECT_FALSE(is_complete(root / "web"));  // the pages are copied first; their script is not there yet
  write(root / "web" / "assets" / "app.js");
  EXPECT_TRUE(is_complete(root / "web"));
}

TEST_F(WebInstall, TheOldFolderComesBackWhenTheLiveOneIsGone) {
  make_complete(root / "web.previous", "old");
  EXPECT_TRUE(recover_previous(root / "web"));
  EXPECT_TRUE(is_complete(root / "web"));
  EXPECT_FALSE(fs::exists(root / "web.previous"));
  std::ifstream in(root / "web" / "assets" / "app.js");
  std::string content;
  in >> content;
  EXPECT_EQ(content, "old");
}

TEST_F(WebInstall, ABrokenLiveFolderIsReplacedByTheCompleteOldOne) {
  write(root / "web" / "assets" / "half.js");
  make_complete(root / "web.previous", "old");
  EXPECT_TRUE(recover_previous(root / "web"));
  EXPECT_TRUE(is_complete(root / "web"));
  EXPECT_FALSE(fs::exists(root / "web" / "assets" / "half.js"));
}

TEST_F(WebInstall, AFolderThatIsBeingCopiedIsLeftToTheInstaller) {
  // pages and a script are there but the end marker is not: the installer is still copying, or stopped
  // in the middle; either way the host must not take the folder away. The next installer run settles it.
  make_complete(root / "web", "half-new");
  make_complete(root / "web.previous", "old");
  EXPECT_FALSE(recover_previous(root / "web"));
  std::ifstream in(root / "web" / "assets" / "app.js");
  std::string content;
  in >> content;
  EXPECT_EQ(content, "half-new");
  EXPECT_TRUE(fs::exists(root / "web.previous"));
}

TEST_F(WebInstall, AFinishedLiveFolderIsNeverTouched) {
  make_marked(root / "web", "new");
  make_complete(root / "web.previous", "old");
  EXPECT_FALSE(recover_previous(root / "web"));
  EXPECT_TRUE(fs::exists(root / "web.previous"));
  std::ifstream in(root / "web" / "assets" / "app.js");
  std::string content;
  in >> content;
  EXPECT_EQ(content, "new");
}

TEST_F(WebInstall, WithoutAnOldFolderNothingIsDone) {
  make_complete(root / "web", "installed-by-an-older-installer");
  EXPECT_FALSE(recover_previous(root / "web"));
  EXPECT_TRUE(is_complete(root / "web"));
}

TEST_F(WebInstall, AnIncompleteOldFolderIsNotUsed) {
  write(root / "web.previous" / "index.html");
  EXPECT_FALSE(recover_previous(root / "web"));
  EXPECT_FALSE(fs::exists(root / "web"));
}

TEST_F(WebInstall, ATrailingSeparatorNamesTheSameFolder) {
  EXPECT_EQ(previous_dir_of(root / "web"), previous_dir_of(root / "web" / ""));
}

TEST_F(WebInstall, TheNoticeSaysInBothLanguagesWhatIsWrongAndWhatToDo) {
  const auto notice = notice_html();
  EXPECT_NE(notice.find("安装不完整"), std::string::npos);
  EXPECT_NE(notice.find("修复"), std::string::npos);
  EXPECT_NE(notice.find("installation is incomplete"), std::string::npos);
  EXPECT_NE(notice.find("Repair"), std::string::npos);
  const auto page = incomplete_page();
  EXPECT_NE(page.find(notice), std::string::npos);
  EXPECT_EQ(page.find("<script"), std::string::npos);
}
