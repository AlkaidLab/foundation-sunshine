/**
 * @file tests/unit/test_gpu_driver_guard.cpp
 * @brief Graphics driver modules, the driver mark, the crash-loop guard and the notice state.
 */
#include "src/gpu_driver_guard.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using crash::gpu_driver::phase_e;
using crash::gpu_driver::vendor_e;
using crash::gpu_driver::vendor_of_module_static;

namespace {
  fs::path
  scratch(const std::string &name) {
    const auto dir = fs::temp_directory_path() / ("gpu_driver_guard_" + name + "_" + std::to_string(std::rand()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
  }

  /// Clocks the test moves by hand. `now` moves both of them; `wall_skew` moves only the wall clock, the way a changed date does.
  struct clock_t_ {
    std::int64_t now = 1'000'000;
    std::int64_t wall_skew = 0;
    gpu_guard::guard_t::clock_fn
    fn() {
      return [this] { return now + wall_skew; };
    }
    gpu_guard::guard_t::clock_fn
    mono() {
      return [this] { return now; };
    }
  };
}  // namespace

// --- modules ---------------------------------------------------------------------------------------------------------------------------------------

TEST(GpuDriverModules, IntelNamesFromTheCrashReports) {
  for (const char *name : { "igd12umd64.dll", "igd10iumd64.dll", "igdumdim64.dll", "igxelpicd64.dll", "igxeumd64.dll", "igc64.dll", "igvk64.dll", "igdgmm64.dll", "IGD12UMD64.DLL" }) {
    EXPECT_EQ(vendor_of_module_static(name), vendor_e::intel) << name;
  }
}

TEST(GpuDriverModules, NvidiaNames) {
  for (const char *name : { "nvwgf2umx.dll", "nvwgf2um.dll", "nvd3dumx.dll", "nvEncodeAPI64.dll", "nvcuvid.dll", "nvcuda.dll", "nvapi64.dll", "libnvidia-encode.so.1", "libGLX_nvidia.so.0" }) {
    EXPECT_EQ(vendor_of_module_static(name), vendor_e::nvidia) << name;
  }
}

TEST(GpuDriverModules, AmdNames) {
  for (const char *name : { "amdxx64.dll", "atiu9p64.dll", "atiumd64.dll", "atidxx64.dll", "aticfx64.dll", "amdvlk64.dll", "amfrt64.dll", "amdgpu_dri.so", "libvulkan_radeon.so" }) {
    EXPECT_EQ(vendor_of_module_static(name), vendor_e::amd) << name;
  }
}

TEST(GpuDriverModules, OtherModulesAreNotDrivers) {
  for (const char *name : { "sunshine.exe", "sunshinesvc.exe", "ntdll.dll", "KERNELBASE.dll", "d3d11.dll", "dxgi.dll", "d3d12.dll", "nvenc.dll.bak.txt", "libx264-164.dll", "avcodec-61.dll", "libcurl-4.dll", "unknown", "" }) {
    EXPECT_EQ(vendor_of_module_static(name), vendor_e::none) << name;
  }
}

TEST(GpuDriverModules, ADirectoryInTheNameIsIgnored) {
  EXPECT_EQ(vendor_of_module_static("C:\\Windows\\System32\\DriverStore\\FileRepository\\x\\nvwgf2umx.dll"), vendor_e::nvidia);
  // the directory name must not decide: a user folder called igd... holds a host module
  EXPECT_EQ(vendor_of_module_static("C:\\Users\\igdfan\\sunshine.exe"), vendor_e::none);
}

TEST(GpuDriverModules, EveryTableEntryIsLowerCaseAndNamesOneVendor) {
  using namespace crash::gpu_driver::detail;
  const auto check = [](const auto &list, vendor_e vendor) {
    for (const auto prefix : list) {
      for (const char c : prefix) EXPECT_FALSE(c >= 'A' && c <= 'Z') << prefix;
      EXPECT_EQ(vendor_of_module_static(prefix), vendor) << prefix;
    }
  };
  check(intel_prefixes, vendor_e::intel);
  check(nvidia_prefixes, vendor_e::nvidia);
  check(amd_prefixes, vendor_e::amd);
}

TEST(GpuDriverModules, TestModuleStandsInForADriver) {
  EXPECT_EQ(crash::gpu_driver::vendor_of_module("host_stand_in.exe"), vendor_e::none);
  crash::gpu_driver::set_test_module("Host_Stand_In.exe", vendor_e::amd);
  EXPECT_EQ(crash::gpu_driver::vendor_of_module("host_stand_in.exe"), vendor_e::amd);
  EXPECT_EQ(crash::gpu_driver::vendor_of_module("other.exe"), vendor_e::none);
  crash::gpu_driver::set_test_module("", vendor_e::none);
  EXPECT_EQ(crash::gpu_driver::vendor_of_module("host_stand_in.exe"), vendor_e::none);
}

// --- the mark ------------------------------------------------------------------------------------------------------------------------------------------

TEST(GpuDriverMark, RendersTheFieldsAndNothingPersonal) {
  crash::gpu_driver::set_driver_version(vendor_e::intel, "32.0.101.6325");
  crash::gpu_driver::set_phase(phase_e::probe);
  const auto mark = crash::gpu_driver::make_mark(vendor_e::intel, "C:\\Users\\someone\\igd12umd64.dll", "crash", 1731889200);
  char text[crash::gpu_driver::mark_buffer_size];
  const auto len = crash::gpu_driver::render_mark(mark, text, sizeof(text));
  ASSERT_GT(len, 0u);
  const auto j = nlohmann::json::parse(std::string(text, len));
  EXPECT_EQ(j["vendor"], "intel");
  EXPECT_EQ(j["module"], "igd12umd64.dll");  // the file name, not the path
  EXPECT_EQ(j["driver"], "32.0.101.6325");
  EXPECT_EQ(j["phase"], "probe");
  EXPECT_EQ(j["kind"], "crash");
  EXPECT_EQ(j["time"], 1731889200);
  EXPECT_EQ(std::string(text, len).find("someone"), std::string::npos);
  crash::gpu_driver::set_phase(phase_e::startup);
}

TEST(GpuDriverMark, AnyReportKindThatIsNotAHangIsACrash) {
  for (const char *kind : { "crash", "abort", "panic", "" }) {
    const auto mark = crash::gpu_driver::make_mark(vendor_e::amd, "amdxx64.dll", kind, 1);
    EXPECT_STREQ(mark.kind, "crash") << kind;
  }
  EXPECT_STREQ(crash::gpu_driver::make_mark(vendor_e::amd, "amdxx64.dll", "hang", 1).kind, "hang");
}

TEST(GpuDriverMark, OddCharactersBecomeUnderscores) {
  crash::gpu_driver::mark_t mark;
  mark.vendor = vendor_e::nvidia;
  std::snprintf(mark.module, sizeof(mark.module), "bad\"name\\x\n.dll");
  std::snprintf(mark.kind, sizeof(mark.kind), "hang");
  char text[crash::gpu_driver::mark_buffer_size];
  const auto len = crash::gpu_driver::render_mark(mark, text, sizeof(text));
  ASSERT_GT(len, 0u);
  const auto j = nlohmann::json::parse(std::string(text, len));
  EXPECT_EQ(j["module"], "bad_name_x_.dll");
}

TEST(GpuDriverMark, ATooSmallBufferGivesNothing) {
  crash::gpu_driver::mark_t mark;
  mark.vendor = vendor_e::amd;
  char text[16];
  EXPECT_EQ(crash::gpu_driver::render_mark(mark, text, sizeof(text)), 0u);
}

TEST(GpuDriverMark, DriverVersionKeepsOnlyVersionCharacters) {
  crash::gpu_driver::set_driver_version(vendor_e::nvidia, "572.83 (DCH)/x\\y");
  EXPECT_STREQ(crash::gpu_driver::driver_version_of(vendor_e::nvidia), "572.83DCHxy");
  crash::gpu_driver::set_driver_version(vendor_e::nvidia, "");
  EXPECT_STREQ(crash::gpu_driver::driver_version_of(vendor_e::nvidia), "");
}

TEST(GpuDriverMark, ReadBackFromTheFileTheHandlerWrote) {
  const auto dir = scratch("mark");
  fs::create_directories(dir / "crashes");
  {
    std::ofstream out(dir / "crashes" / crash::gpu_driver::mark_file_name);
    out << R"({"v":1,"vendor":"intel","module":"igd12umd64.dll","driver":"32.0.101.6325","phase":"startup","kind":"hang","time":1731889200})";
  }
  const auto mark = gpu_guard::read_mark(dir);
  ASSERT_TRUE(mark.has_value());
  EXPECT_EQ(mark->vendor, vendor_e::intel);
  EXPECT_EQ(mark->module, "igd12umd64.dll");
  EXPECT_EQ(mark->kind, "hang");
  EXPECT_EQ(mark->phase, phase_e::startup);
  EXPECT_EQ(mark->key(), "intel:32.0.101.6325");
}

TEST(GpuDriverMark, NoFileOrADamagedOneIsNoMark) {
  const auto dir = scratch("nomark");
  EXPECT_FALSE(gpu_guard::read_mark(dir).has_value());
  fs::create_directories(dir / "crashes");
  std::ofstream(dir / "crashes" / crash::gpu_driver::mark_file_name) << "{ not json";
  EXPECT_FALSE(gpu_guard::read_mark(dir).has_value());
  std::ofstream(dir / "crashes" / crash::gpu_driver::mark_file_name) << R"({"vendor":"apple"})";
  EXPECT_FALSE(gpu_guard::read_mark(dir).has_value());
}

// --- the guard ---------------------------------------------------------------------------------------------------------------------------------------

namespace {
  /// One start that ends after `lived` seconds without a shutdown request.
  bool
  start_and_die(const fs::path &dir, clock_t_ &clock, int lived, bool hang = false) {
    gpu_guard::guard_t guard { dir, {}, clock.fn(), clock.mono() };
    const bool safe = guard.begin();
    for (int s = 0; s < lived; ++s) {
      clock.now += 1;
      guard.tick();
    }
    if (hang) guard.note_hang();
    clock.now += 2;  // the restart
    return safe;
  }
}  // namespace

TEST(GpuGuard, ThreeShortRunsInARowPutTheFourthStartInSafeMode) {
  const auto dir = scratch("loop");
  clock_t_ clock;
  EXPECT_FALSE(start_and_die(dir, clock, 3));  // the first start: nothing on record
  EXPECT_FALSE(start_and_die(dir, clock, 6));  // sees one failed run
  EXPECT_FALSE(start_and_die(dir, clock, 2));  // sees two
  EXPECT_TRUE(start_and_die(dir, clock, 4));  // sees three: safe mode
  gpu_guard::guard_t next { dir, {}, clock.fn(), clock.mono() };
  EXPECT_TRUE(next.begin());  // and it stays on while it keeps failing
}

TEST(GpuGuard, ARunThatStaysUpEndsTheCount) {
  const auto dir = scratch("stable");
  clock_t_ clock;
  start_and_die(dir, clock, 3);
  start_and_die(dir, clock, 3);
  start_and_die(dir, clock, 30);  // crashed after 30 s: not a start-up loop
  EXPECT_FALSE(start_and_die(dir, clock, 3));
  EXPECT_FALSE(start_and_die(dir, clock, 3));
  EXPECT_FALSE(start_and_die(dir, clock, 3));
  EXPECT_TRUE(start_and_die(dir, clock, 3));
}

TEST(GpuGuard, AShutdownRequestIsNotAFailure) {
  const auto dir = scratch("clean");
  clock_t_ clock;
  for (int i = 0; i < 6; ++i) {
    gpu_guard::guard_t guard { dir, {}, clock.fn(), clock.mono() };
    EXPECT_FALSE(guard.begin());
    clock.now += 1;
    guard.note_clean_exit();  // a restart from the settings, 1 s after the start
  }
}

TEST(GpuGuard, AHangInTheDriverCountsEvenWhenTheProcessStaysUp) {
  const auto dir = scratch("hang");
  clock_t_ clock;
  start_and_die(dir, clock, 60, true);
  start_and_die(dir, clock, 60, true);
  start_and_die(dir, clock, 60, true);
  EXPECT_TRUE(start_and_die(dir, clock, 60, true));
}

TEST(GpuGuard, AHangThatEndedInAShutdownRequestDoesNotCount) {
  const auto dir = scratch("hang-clean");
  clock_t_ clock;
  for (int i = 0; i < 5; ++i) {
    gpu_guard::guard_t guard { dir, {}, clock.fn(), clock.mono() };
    EXPECT_FALSE(guard.begin());
    guard.note_hang();
    guard.note_clean_exit();
    clock.now += 40;
  }
}

TEST(GpuGuard, SafeModeEndsAfterFiveMinutes) {
  const auto dir = scratch("five");
  clock_t_ clock;
  for (int i = 0; i < 3; ++i) start_and_die(dir, clock, 2);
  gpu_guard::guard_t guard { dir, {}, clock.fn(), clock.mono() };
  ASSERT_TRUE(guard.begin());
  for (int s = 0; s < 299; ++s) {
    clock.now += 1;
    guard.tick();
  }
  EXPECT_TRUE(guard.safe_mode());
  clock.now += 1;
  guard.tick();
  EXPECT_FALSE(guard.safe_mode());
  EXPECT_EQ(guard.snapshot().consecutive, 0);
  gpu_guard::guard_t after { dir, {}, clock.fn(), clock.mono() };
  EXPECT_FALSE(after.begin());  // written to disk
}

TEST(GpuGuard, SafeModeEndsWhenAStreamHasEnded) {
  const auto dir = scratch("stream");
  clock_t_ clock;
  for (int i = 0; i < 3; ++i) start_and_die(dir, clock, 2);
  gpu_guard::guard_t guard { dir, {}, clock.fn(), clock.mono() };
  ASSERT_TRUE(guard.begin());
  clock.now += 20;
  guard.tick();
  EXPECT_TRUE(guard.safe_mode());  // a stable run alone does not end it
  guard.note_stream_finished();
  EXPECT_FALSE(guard.safe_mode());
}

TEST(GpuGuard, AStreamEndingOutsideSafeModeChangesNothing) {
  const auto dir = scratch("stream-plain");
  clock_t_ clock;
  gpu_guard::guard_t guard { dir, {}, clock.fn(), clock.mono() };
  EXPECT_FALSE(guard.begin());
  guard.note_stream_finished();
  EXPECT_FALSE(guard.safe_mode());
}

TEST(GpuGuard, ManualRetryLeavesSafeModeAndOneMoreFailureBringsItBack) {
  const auto dir = scratch("retry");
  clock_t_ clock;
  for (int i = 0; i < 3; ++i) start_and_die(dir, clock, 2);
  {
    gpu_guard::guard_t guard { dir, {}, clock.fn(), clock.mono() };
    ASSERT_TRUE(guard.begin());
    EXPECT_TRUE(guard.request_retry());
    EXPECT_FALSE(guard.safe_mode());
    EXPECT_FALSE(guard.request_retry());  // not in safe mode any more
    clock.now += 3;  // dies at once
  }
  gpu_guard::guard_t next { dir, {}, clock.fn(), clock.mono() };
  EXPECT_TRUE(next.begin());
}

TEST(GpuGuard, ManualRetryAfterALongRunWatchesTheTryFromItsStart) {
  const auto dir = scratch("retry-late");
  clock_t_ clock;
  for (int i = 0; i < 3; ++i) start_and_die(dir, clock, 2);
  {
    gpu_guard::guard_t guard { dir, {}, clock.fn(), clock.mono() };
    ASSERT_TRUE(guard.begin());
    for (int s = 0; s < 12; ++s) {  // the run is already stable when the person asks for the try
      clock.now += 1;
      guard.tick();
    }
    ASSERT_TRUE(guard.snapshot().run.stable);
    EXPECT_TRUE(guard.request_retry());
    EXPECT_FALSE(guard.snapshot().run.stable);
    clock.now += 2;  // the try fails at once
  }
  gpu_guard::guard_t next { dir, {}, clock.fn(), clock.mono() };
  EXPECT_TRUE(next.begin());  // back in the safe mode
}

TEST(GpuGuard, ManualRetryThatWorksResetsTheCount) {
  const auto dir = scratch("retry-ok");
  clock_t_ clock;
  for (int i = 0; i < 3; ++i) start_and_die(dir, clock, 2);
  {
    gpu_guard::guard_t guard { dir, {}, clock.fn(), clock.mono() };
    ASSERT_TRUE(guard.begin());
    guard.request_retry();
    for (int s = 0; s < 12; ++s) {
      clock.now += 1;
      guard.tick();
    }
    clock.now += 5;
  }
  gpu_guard::guard_t next { dir, {}, clock.fn(), clock.mono() };
  EXPECT_FALSE(next.begin());
  EXPECT_EQ(next.snapshot().consecutive, 0);
}

TEST(GpuGuard, ADamagedFileStartsFresh) {
  const auto dir = scratch("damaged");
  std::ofstream(dir / gpu_guard::GUARD_FILE) << "garbage {{";
  clock_t_ clock;
  gpu_guard::guard_t guard { dir, {}, clock.fn(), clock.mono() };
  EXPECT_FALSE(guard.begin());
}

TEST(GpuGuard, ThePolicyIsConfigurable) {
  const auto dir = scratch("policy");
  clock_t_ clock;
  gpu_guard::policy_t policy;
  policy.loop_threshold = 2;
  policy.stable_after_s = 5;
  for (int i = 0; i < 2; ++i) {
    gpu_guard::guard_t guard { dir, policy, clock.fn(), clock.mono() };
    guard.begin();
    clock.now += 4;
    guard.tick();
    clock.now += 1;
  }
  gpu_guard::guard_t guard { dir, policy, clock.fn(), clock.mono() };
  EXPECT_TRUE(guard.begin());
}

// --- the clocks --------------------------------------------------------------------------------------------------------------------------------------

TEST(GpuGuardClocks, AWallClockSetForwardDoesNotMakeAShortRunStable) {
  const auto dir = scratch("skew-forward");
  clock_t_ clock;
  for (int i = 0; i < 3; ++i) {
    gpu_guard::guard_t guard { dir, {}, clock.fn(), clock.mono() };
    EXPECT_FALSE(guard.begin());
    clock.now += 2;
    clock.wall_skew += 86'400;  // a time sync sets the date a day forward while the host is still starting
    guard.tick();
    EXPECT_FALSE(guard.snapshot().run.stable);  // two seconds have passed, whatever the date says
    clock.now += 1;  // the crash and the restart
  }
  gpu_guard::guard_t next { dir, {}, clock.fn(), clock.mono() };
  EXPECT_TRUE(next.begin());  // three failed starts in a row: the protection still holds
}

TEST(GpuGuardClocks, AWallClockSetBackDoesNotKeepARunFromBecomingStable) {
  const auto dir = scratch("skew-back");
  clock_t_ clock;
  gpu_guard::guard_t guard { dir, {}, clock.fn(), clock.mono() };
  ASSERT_FALSE(guard.begin());
  clock.wall_skew -= 3'600;  // the date goes an hour back
  for (int s = 0; s < 12; ++s) {
    clock.now += 1;
    guard.tick();
  }
  EXPECT_TRUE(guard.snapshot().run.stable);  // it has stayed up for 12 s
  gpu_guard::guard_t next { dir, {}, clock.fn(), clock.mono() };
  EXPECT_FALSE(next.begin());
  EXPECT_EQ(next.snapshot().consecutive, 0);  // and the next start does not count it as a failure
}

TEST(GpuGuardClocks, SafeModeEndsAfterFiveMinutesOfRunningWhateverTheDateDoes) {
  const auto dir = scratch("skew-safe");
  clock_t_ clock;
  for (int i = 0; i < 3; ++i) start_and_die(dir, clock, 2);
  gpu_guard::guard_t guard { dir, {}, clock.fn(), clock.mono() };
  ASSERT_TRUE(guard.begin());
  clock.now += 10;
  clock.wall_skew += 7 * 86'400;  // a week forward
  guard.tick();
  EXPECT_TRUE(guard.safe_mode());  // ten seconds of running do not end it
  clock.now += 290;
  clock.wall_skew -= 14 * 86'400;  // and two weeks back
  guard.tick();
  EXPECT_FALSE(guard.safe_mode());  // 300 s of running do
}

TEST(GpuGuardClocks, ARetryIsWatchedOnTheRunningTimeToo) {
  const auto dir = scratch("skew-retry");
  clock_t_ clock;
  for (int i = 0; i < 3; ++i) start_and_die(dir, clock, 2);
  gpu_guard::guard_t guard { dir, {}, clock.fn(), clock.mono() };
  ASSERT_TRUE(guard.begin());
  clock.now += 60;
  ASSERT_TRUE(guard.request_retry());
  clock.wall_skew += 86'400;
  clock.now += 3;
  guard.tick();
  EXPECT_FALSE(guard.snapshot().run.stable);  // three seconds into the try, not a day
  clock.now += 8;
  guard.tick();
  EXPECT_TRUE(guard.snapshot().run.stable);
}

TEST(GpuGuardClocks, ADefaultGuardMeasuresOnTheMonotonicClock) {
  const auto dir = scratch("default-clock");
  const auto a = gpu_guard::monotonic_now();
  const auto b = gpu_guard::monotonic_now();
  EXPECT_LE(a, b);
  gpu_guard::guard_t guard { dir };
  EXPECT_FALSE(guard.begin());
  guard.tick();  // runs on the real clocks without waiting
  EXPECT_FALSE(guard.snapshot().run.stable);
  EXPECT_NE(guard.snapshot().run.start, 0);
}

// --- the notice -------------------------------------------------------------------------------------------------------------------------------------------

namespace {
  gpu_guard::driver_mark_t
  intel_mark(const std::string &driver) {
    gpu_guard::driver_mark_t mark;
    mark.vendor = vendor_e::intel;
    mark.module = "igd12umd64.dll";
    mark.driver = driver;
    mark.kind = "crash";
    return mark;
  }
}  // namespace

TEST(GpuNotice, ShownUntilDismissedAndOnlyOncePerDriverVersion) {
  const auto dir = scratch("notice");
  gpu_guard::notice_store_t store { dir };
  const auto mark = intel_mark("32.0.101.6325");
  EXPECT_TRUE(gpu_guard::notice_due(mark, "32.0.101.6325", store));
  store.dismiss(mark.key());
  EXPECT_FALSE(gpu_guard::notice_due(mark, "32.0.101.6325", store));
  // the same version failing again does not bring it back; another version does
  EXPECT_FALSE(gpu_guard::notice_due(intel_mark("32.0.101.6325"), "32.0.101.6325", store));
  EXPECT_TRUE(gpu_guard::notice_due(intel_mark("32.0.101.6881"), "32.0.101.6881", store));
}

TEST(GpuNotice, ADriverThatWasUpdatedSinceTheMarkIsNotBlamedAgain) {
  const auto dir = scratch("updated");
  gpu_guard::notice_store_t store { dir };
  EXPECT_FALSE(gpu_guard::notice_due(intel_mark("32.0.101.6325"), "32.0.101.6881", store));
  // unknown on either side: the notice stays
  EXPECT_TRUE(gpu_guard::notice_due(intel_mark("32.0.101.6325"), "", store));
  EXPECT_TRUE(gpu_guard::notice_due(intel_mark(""), "32.0.101.6881", store));
}

TEST(GpuNotice, DismissalsSurviveARestartAndStayBounded) {
  const auto dir = scratch("persist");
  {
    gpu_guard::notice_store_t store { dir };
    store.dismiss("intel:1");
    store.mark_notified("intel:1");
  }
  gpu_guard::notice_store_t again { dir };
  EXPECT_TRUE(again.dismissed("intel:1"));
  EXPECT_TRUE(again.notified("intel:1"));
  EXPECT_FALSE(again.dismissed("intel:2"));
  for (int i = 0; i < 100; ++i) again.dismiss("amd:" + std::to_string(i));
  std::ifstream in(dir / gpu_guard::NOTICE_FILE);
  const auto j = nlohmann::json::parse(in);
  EXPECT_LE(j["dismissed"].size(), 32u);
  EXPECT_TRUE(again.dismissed("amd:99"));
}

TEST(GpuStatus, SafeModeAndTheDriverShowInTheStatus) {
  const auto dir = scratch("status");
  clock_t_ clock;
  for (int i = 0; i < 3; ++i) start_and_die(dir, clock, 2);
  gpu_guard::guard_t guard { dir, {}, clock.fn(), clock.mono() };
  guard.begin();
  gpu_guard::notice_store_t store { dir };
  const auto j = gpu_guard::status_json(&guard, gpu_guard::probe_e::skipped, intel_mark("32.0.101.6325"), "32.0.101.6325", store);
  EXPECT_EQ(j["probe"], "skipped");
  EXPECT_TRUE(j["safe_mode"]["active"].get<bool>());
  EXPECT_EQ(j["safe_mode"]["auto_exit_s"], 300);
  ASSERT_EQ(j["safe_mode"]["skipped"].size(), 2u);
  EXPECT_EQ(j["safe_mode"]["skipped"][0], "encoder_probe");
  EXPECT_EQ(j["safe_mode"]["skipped"][1], "display_restore");
  EXPECT_TRUE(j["driver"]["show"].get<bool>());
  EXPECT_EQ(j["driver"]["vendor"], "intel");
  EXPECT_EQ(j["driver"]["key"], "intel:32.0.101.6325");
}

TEST(GpuStatus, NothingToSayWithoutAGuardOrAMark) {
  const auto dir = scratch("status-empty");
  gpu_guard::notice_store_t store { dir };
  const auto j = gpu_guard::status_json(nullptr, gpu_guard::probe_e::pending, std::nullopt, "", store);
  EXPECT_EQ(j["probe"], "pending");
  EXPECT_FALSE(j["safe_mode"]["active"].get<bool>());
  EXPECT_TRUE(j["driver"].is_null());
}
