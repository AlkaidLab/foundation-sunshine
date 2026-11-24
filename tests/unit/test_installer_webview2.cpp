#include "tools/installer_webview2.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using namespace installer_webview2;

namespace {

  // A machine for ensure_runtime: what is installed, what each step does, and which steps were called, in order.
  struct fixture {
    bool present = false;
    bool download_ok = true;
    bool signature_ok = true;
    bool run_ok = true;
    bool runtime_after_run = true;
    unsigned long download_code = 12007;  // ERROR_WINHTTP_TIMEOUT
    unsigned long signature_code = 0x800B0100;
    unsigned long run_code = 0;
    std::vector<std::string> calls;

    steps
    make() {
      steps s;
      s.runtime_present = [this] {
        calls.push_back("present?");
        return present;
      };
      s.download = [this](unsigned long *code) {
        calls.push_back("download");
        if (!download_ok) *code = download_code;
        return download_ok;
      };
      s.verify_signature = [this](unsigned long *code) {
        calls.push_back("verify");
        if (!signature_ok) *code = signature_code;
        return signature_ok;
      };
      s.run_bootstrapper = [this](unsigned long *code) {
        calls.push_back("run");
        *code = run_code;
        if (run_ok && runtime_after_run) present = true;
        return run_ok;
      };
      s.discard = [this] { calls.push_back("discard"); };
      s.progress = [this](std::string_view what) { calls.push_back("progress:" + std::string(what)); };
      return s;
    }
  };

  bool
  called(const fixture &f, const std::string &name) {
    for (const auto &c : f.calls)
      if (c == name) return true;
    return false;
  }

}  // namespace

// The three cases of the release gate: installed, missing and downloaded, the download fails.

TEST(InstallerWebView2, anInstalledRuntimeIsLeftAloneAndNothingIsFetched) {
  fixture f;
  f.present = true;
  const auto r = ensure_runtime(f.make());
  EXPECT_EQ(r.what, outcome::already_installed);
  EXPECT_TRUE(runtime_available(r.what));
  EXPECT_EQ(f.calls, std::vector<std::string>({"present?"}));
  EXPECT_EQ(result_fields(r), ",\"webview2\":\"present\"");
}

TEST(InstallerWebView2, aMissingRuntimeIsDownloadedCheckedRunAndTheDownloadRemoved) {
  fixture f;
  const auto r = ensure_runtime(f.make());
  EXPECT_EQ(r.what, outcome::installed);
  EXPECT_EQ(r.code, 0u);
  EXPECT_EQ(f.calls, std::vector<std::string>({"present?", "progress:download", "download", "verify", "progress:install", "run", "discard", "present?"}));
  EXPECT_EQ(result_fields(r), ",\"webview2\":\"installed\"");
}

TEST(InstallerWebView2, aFailedDownloadIsReportedWithTheReasonTheCodeAndTheManualLinkAndRunsNothing) {
  fixture f;
  f.download_ok = false;
  const auto r = ensure_runtime(f.make());
  EXPECT_EQ(r.what, outcome::download_failed);
  EXPECT_FALSE(runtime_available(r.what));
  EXPECT_EQ(r.code, 12007u);
  EXPECT_FALSE(called(f, "verify"));
  EXPECT_FALSE(called(f, "run"));
  EXPECT_TRUE(called(f, "discard"));  // a partial file does not stay in the run directory
  const std::string fields = result_fields(r);
  EXPECT_NE(fields.find("\"webview2\":\"missing\""), std::string::npos);
  EXPECT_NE(fields.find("\"webview2_reason\":\"download_failed\""), std::string::npos);
  EXPECT_NE(fields.find("\"webview2_code\":12007"), std::string::npos);
  EXPECT_NE(fields.find("\"webview2_manual_url\":\"https://go.microsoft.com/fwlink/p/?LinkId=2124703\""), std::string::npos);
}

// What the three cases do not say.

TEST(InstallerWebView2, aFileThatIsNotSignedByMicrosoftIsNeverRunAndIsRemoved) {
  fixture f;
  f.signature_ok = false;
  const auto r = ensure_runtime(f.make());
  EXPECT_EQ(r.what, outcome::signature_invalid);
  EXPECT_EQ(r.code, 0x800B0100ul);
  EXPECT_FALSE(called(f, "run"));
  EXPECT_TRUE(called(f, "discard"));
  EXPECT_NE(result_fields(r).find("\"webview2_reason\":\"signature_invalid\""), std::string::npos);
}

TEST(InstallerWebView2, aBootstrapperThatCannotStartIsAFailureWithACode) {
  fixture f;
  f.run_ok = false;
  f.run_code = 5;  // ERROR_ACCESS_DENIED from CreateProcess
  const auto r = ensure_runtime(f.make());
  EXPECT_EQ(r.what, outcome::install_failed);
  EXPECT_EQ(r.code, 5u);
  EXPECT_TRUE(called(f, "discard"));
}

TEST(InstallerWebView2, aBootstrapperThatLeavesNoRuntimeIsAFailureWhateverItsExitCode) {
  fixture f;
  f.runtime_after_run = false;
  f.run_code = 0;
  EXPECT_EQ(ensure_runtime(f.make()).what, outcome::install_failed);
  fixture g;
  g.runtime_after_run = false;
  g.run_code = 0x80040154;
  const auto r = ensure_runtime(g.make());
  EXPECT_EQ(r.what, outcome::install_failed);
  EXPECT_EQ(r.code, 0x80040154ul);
}

TEST(InstallerWebView2, aFailureAlwaysHasANonZeroCode) {
  fixture f;
  f.run_ok = false;
  f.run_code = 0;
  EXPECT_NE(ensure_runtime(f.make()).code, 0u);
}

TEST(InstallerWebView2, theRegistryDecidesNotTheExitCode) {
  fixture f;
  f.run_code = 1;  // the bootstrapper exited with an error code, but the Runtime is registered afterwards
  EXPECT_EQ(ensure_runtime(f.make()).what, outcome::installed);
}

TEST(InstallerWebView2, theStepsMayLackTheOptionalHooks) {
  fixture f;
  steps s = f.make();
  s.discard = nullptr;
  s.progress = nullptr;
  EXPECT_EQ(ensure_runtime(s).what, outcome::installed);
  f.present = false;
  f.signature_ok = false;
  EXPECT_EQ(ensure_runtime(s).what, outcome::signature_invalid);
}

TEST(InstallerWebView2, pvOfTheClientKeyDecidesWhetherTheRuntimeIsThere) {
  EXPECT_TRUE(pv_means_installed(L"131.0.2903.112"));
  EXPECT_TRUE(pv_means_installed(L"1.0.0.0"));
  EXPECT_FALSE(pv_means_installed(L""));
  EXPECT_FALSE(pv_means_installed(L"0.0.0.0"));
}

TEST(InstallerWebView2, theSignerMustBeExactlyMicrosoftCorporation) {
  EXPECT_TRUE(signer_is_microsoft(L"Microsoft Corporation"));
  EXPECT_FALSE(signer_is_microsoft(L""));
  EXPECT_FALSE(signer_is_microsoft(L"Microsoft Corporation Ltd"));
  EXPECT_FALSE(signer_is_microsoft(L"Not Microsoft Corporation"));
  EXPECT_FALSE(signer_is_microsoft(L"microsoft corporation"));
  EXPECT_FALSE(signer_is_microsoft(L"Microsoft Windows"));
  EXPECT_FALSE(signer_is_microsoft(std::wstring_view(L"Microsoft Corporation\0Evil", 26)));
}

TEST(InstallerWebView2, theDownloadIsMicrosoftsFixedLinkOverHttpsAndBounded) {
  EXPECT_EQ(kBootstrapperUrl.substr(0, 8), "https://");
  EXPECT_EQ(kBootstrapperUrl, "https://go.microsoft.com/fwlink/p/?LinkId=2124703");
  EXPECT_GE(kMaxDownloadBytes, 4ull * 1024 * 1024);  // room for a bigger bootstrapper
  EXPECT_LE(kMaxDownloadBytes, 32ull * 1024 * 1024);
}

TEST(InstallerWebView2, everyOutcomeHasItsOwnWord) {
  const outcome all[] = {outcome::already_installed, outcome::installed, outcome::download_failed, outcome::signature_invalid, outcome::install_failed};
  std::vector<std::string> words;
  for (const auto o : all) words.emplace_back(outcome_word(o));
  for (size_t i = 0; i < words.size(); ++i)
    for (size_t j = i + 1; j < words.size(); ++j) EXPECT_NE(words[i], words[j]);
}
