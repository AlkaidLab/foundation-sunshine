#pragma once
// The WebView2 Runtime step of the install worker, without any Windows header: the installer shell implements the steps
// (WinHTTP download, WinVerifyTrust, the bootstrapper process, the registry) and tests/unit/test_installer_webview2.cpp
// drives the same decision with fakes. The Panel runs on WebView2, but the installer no longer carries Microsoft's
// bootstrapper (1.9 MB, 1.8 MB of the download): a machine without the Runtime fetches it from Microsoft when it is needed.
//
// Authority: installer-ms-webview2-distribution-20261109 (Microsoft Learn, "Distribute your app and the WebView2 Runtime":
// the fixed bootstrapper link, `MicrosoftEdgeWebview2Setup.exe /silent /install`, the pv registry value) and
// installer-ms-winverifytrust-20261118 (WinVerifyTrust, WINTRUST_ACTION_GENERIC_VERIFY_V2). Microsoft replaces the
// bootstrapper behind the link from time to time, so its hash is not pinned: its Authenticode signature is checked, and
// the signer organisation must be exactly "Microsoft Corporation".
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace installer_webview2 {

  /** Microsoft's fixed link of the Evergreen Bootstrapper. Also what a failed install tells the person to download by hand. */
  inline constexpr std::string_view kBootstrapperUrl = "https://go.microsoft.com/fwlink/p/?LinkId=2124703";
  /** The organisation (O=) of the leaf certificate that must have signed the bootstrapper. Exact, not a substring. */
  inline constexpr std::wstring_view kRequiredSigner = L"Microsoft Corporation";
  /** The bootstrapper is 1.9 MB; a response much larger than that is not it. */
  inline constexpr std::uint64_t kMaxDownloadBytes = 8ull * 1024 * 1024;

  /** The pv value of the Evergreen client key: empty and 0.0.0.0 both mean "not installed" (Microsoft Learn, detection). */
  inline bool
  pv_means_installed(std::wstring_view pv) {
    return !pv.empty() && pv != L"0.0.0.0";
  }

  inline bool
  signer_is_microsoft(std::wstring_view organization) {
    return organization == kRequiredSigner;
  }

  enum class outcome {
    already_installed,  // the Runtime was there: nothing is downloaded or run
    installed,          // downloaded, signature checked, bootstrapper run, the Runtime is there now
    download_failed,    // no network, a proxy, a bad status, a response too large
    signature_invalid,  // not signed, a broken signature, or not signed by Microsoft Corporation: never run
    install_failed,     // the bootstrapper did not start, ran into the time limit, or left no Runtime behind
  };

  struct result {
    outcome what = outcome::already_installed;
    unsigned long code = 0;  // the Win32 error or the bootstrapper's exit code of the failed step, 0 otherwise
  };

  inline bool
  runtime_available(const outcome what) {
    return what == outcome::already_installed || what == outcome::installed;
  }

  /** The word of an outcome in events and in install-result.json. */
  inline std::string_view
  outcome_word(const outcome what) {
    switch (what) {
      case outcome::already_installed:
        return "present";
      case outcome::installed:
        return "installed";
      case outcome::download_failed:
        return "download_failed";
      case outcome::signature_invalid:
        return "signature_invalid";
      case outcome::install_failed:
        return "install_failed";
    }
    return "install_failed";
  }

  /** The fields appended to install-result.json. A missing Runtime carries the reason, its code and the manual download link: the done page and the Panel read it. */
  inline std::string
  result_fields(const result &r) {
    if (runtime_available(r.what)) return ",\"webview2\":\"" + std::string(outcome_word(r.what)) + "\"";
    return ",\"webview2\":\"missing\",\"webview2_reason\":\"" + std::string(outcome_word(r.what)) + "\",\"webview2_code\":" +
           std::to_string(r.code) + ",\"webview2_manual_url\":\"" + std::string(kBootstrapperUrl) + "\"";
  }

  /** What the worker implements. Every step reports a failure code through its argument; none of them throws. */
  struct steps {
    std::function<bool()> runtime_present;
    std::function<bool(unsigned long *)> download;           // the bootstrapper into a file of the protected run directory
    std::function<bool(unsigned long *)> verify_signature;   // Authenticode valid and signed by kRequiredSigner
    std::function<bool(unsigned long *)> run_bootstrapper;   // started and finished; the exit code does not decide, the registry does
    std::function<void()> discard;                           // delete the downloaded file
    std::function<void(std::string_view)> progress;          // "download" and "install", for the events file
  };

  /** Install the Runtime if it is missing. A failure never stops the install of the host: the caller records it and goes on. */
  inline result
  ensure_runtime(const steps &s) {
    if (s.runtime_present()) return {outcome::already_installed, 0};
    unsigned long code = 0;
    if (s.progress) s.progress("download");
    if (!s.download(&code)) {
      if (s.discard) s.discard();  // a partial file
      return {outcome::download_failed, code};
    }
    if (!s.verify_signature(&code)) {
      if (s.discard) s.discard();
      return {outcome::signature_invalid, code};
    }
    if (s.progress) s.progress("install");
    code = 0;
    const bool ran = s.run_bootstrapper(&code);
    if (s.discard) s.discard();
    // The bootstrapper's exit code is not trusted either way: what counts is the Runtime being registered afterwards.
    if (s.runtime_present()) return {outcome::installed, 0};
    return {outcome::install_failed, ran ? code : (code != 0 ? code : 1)};
  }

}  // namespace installer_webview2
