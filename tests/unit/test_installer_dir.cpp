#include "tools/installer_dir.h"
#include "tools/installer_json.h"

#include <gtest/gtest.h>

#include <cwchar>
#include <string>

using namespace installer_dir;

namespace {

  // A plain NTFS fixed disk with room, nothing in the way: what a second internal drive looks like.
  facts
  good_disk() {
    facts f;
    f.fixed_disk = true;
    f.file_system = L"NTFS";
    f.free_bytes = 500ull * 1024 * 1024 * 1024;
    f.system_roots = { L"C:\\Windows" };
    f.user_roots = { L"C:\\Users", L"D:\\Documents" };
    f.synced_roots = { L"C:\\Users\\someone\\OneDrive" };
    f.synced_names = default_synced_names();
    return f;
  }

  error
  of(std::wstring_view path, const facts &f = good_disk()) {
    return check(path, f).problem;
  }

}  // namespace

TEST(InstallerDirShape, aFullDrivePathIsAccepted) {
  EXPECT_EQ(look(L"D:\\Sunshine").problem, error::none);
  EXPECT_EQ(look(L"d:/Games/Sunshine").path, L"d:\\Games\\Sunshine") << "forward slashes become backslashes";
  EXPECT_EQ(look(L"D:\\Games\\Sunshine\\").path, L"D:\\Games\\Sunshine") << "a trailing backslash is dropped";
  EXPECT_EQ(look(L"d:\\x").drive, L'D');
  EXPECT_EQ(look(L"D:\\Program Files\\My Games\\Sunshine (host)").problem, error::none) << "spaces and parentheses are fine";
}

TEST(InstallerDirShape, chineseAndOtherNonAsciiPathsAreAccepted) {
  EXPECT_EQ(look(L"D:\\串流\\基地阳光").problem, error::none);
  EXPECT_EQ(look(L"E:\\Приложения\\Sunshine").problem, error::none);
  EXPECT_EQ(look(L"D:\\ゲーム\\Sunshine").problem, error::none);
  EXPECT_EQ(look(L"D:\\串流\\基地阳光").path, L"D:\\串流\\基地阳光") << "nothing is re-encoded or folded";
}

TEST(InstallerDirShape, whatIsNotAFullDrivePathIsRefused) {
  for (const wchar_t *p : { L"", L"Sunshine", L"\\Sunshine", L"D:", L"D:Sunshine", L"1:\\x", L"expand:{pf}\\x", L"D:\\a\\..\\b", L"D:\\a\\.\\b", L"D:\\a\\\\b" })
    EXPECT_EQ(look(p).problem, error::invalid) << std::string(p, p + wcslen(p));
  EXPECT_EQ(look(L"\\\\?\\D:\\Sunshine").problem, error::invalid);
  EXPECT_EQ(look(L"\\\\.\\PhysicalDrive0").problem, error::invalid);
  EXPECT_EQ(look(L"\\\\server\\share\\Sunshine").problem, error::not_fixed) << "a UNC path is a network place";
  EXPECT_TRUE(look(L"\\\\server\\share").unc);
}

TEST(InstallerDirShape, partsWindowsDoesNotAllow) {
  for (const wchar_t *p : { L"D:\\a<b", L"D:\\a>b", L"D:\\a|b", L"D:\\a?b", L"D:\\a*b", L"D:\\a\"b", L"D:\\a:b", L"D:\\a\tb", L"D:\\x\\CON", L"D:\\x\\nul.txt", L"D:\\x\\com1", L"D:\\x\\LPT9.log",
         L"D:\\x\\name ", L"D:\\x\\name.", L"D:\\ x" })
    EXPECT_EQ(look(p).problem, error::invalid) << std::string(p, p + wcslen(p));
  EXPECT_EQ(look(L"D:\\x\\com10").problem, error::none) << "COM10 is an ordinary name";
  for (const wchar_t digit : { wchar_t(0x00B9), wchar_t(0x00B2), wchar_t(0x00B3) }) {
    EXPECT_EQ(look(std::wstring(L"D:\\Apps\\COM") + digit + L".log").problem, error::invalid) << "COM with a superscript digit";
    EXPECT_EQ(look(std::wstring(L"D:\\Apps\\lpt") + digit).problem, error::invalid);
  }
  EXPECT_EQ(look(L"D:\\x\\console").problem, error::none);
}

TEST(InstallerDirShape, charactersTheScriptsCannotCarryAreNamed) {
  // delayed expansion (!), %, ^, &, the PATH separator (;), the engine's constants ({}), and quoting
  for (const wchar_t *p : { L"D:\\a!b", L"D:\\a%b", L"D:\\a^b", L"D:\\a&b", L"D:\\a;b", L"D:\\a,b", L"D:\\a=b", L"D:\\a'b", L"D:\\a`b", L"D:\\{app}", L"D:\\a$b" })
    EXPECT_EQ(look(p).problem, error::chars) << std::string(p, p + wcslen(p));
  EXPECT_EQ(look(L"D:\\a-b_c.d+e@f#g~h[i]j(k)").problem, error::none);
}

TEST(InstallerDirShape, aDiskRootIsNotAFolder) {
  EXPECT_EQ(look(L"D:\\").problem, error::root);
  EXPECT_EQ(look(L"d:").problem, error::invalid);
  EXPECT_EQ(look(L"D:\\\\").problem, error::root) << "trailing backslashes are dropped first";
  EXPECT_TRUE(look(L"E:\\").root);
}

TEST(InstallerDirShape, theLongestPathOfThePackageMustFit) {
  EXPECT_EQ(kMaxLength + 1 + kLongestRelative + kPathMargin, kMaxPath) << "the budget adds up to MAX_PATH";
  std::wstring ok = L"D:\\";
  while (ok.size() + 11 < kMaxLength) ok += L"abcdefghij\\";
  ok.pop_back();
  EXPECT_LE(ok.size(), kMaxLength);
  EXPECT_EQ(look(ok).problem, error::none);
  EXPECT_EQ(look(ok + std::wstring(kMaxLength - ok.size() + 1, L'x')).problem, error::too_long) << "one over";
  EXPECT_EQ(look(ok + std::wstring(kMaxLength - ok.size(), L'x')).problem, error::none) << "exactly at the limit";
  // a path of Chinese characters is counted in UTF-16 units, not in bytes
  EXPECT_EQ(look(L"D:\\" + std::wstring(60, L'串')).problem, error::none);
  EXPECT_EQ(look(L"D:\\" + std::wstring(kMaxLength, L'串')).problem, error::too_long);
  EXPECT_EQ(look(L"D:\\" + std::wstring(10, L'x'), 12).problem, error::too_long) << "the limit is a parameter";
  // a character above U+FFFF is two UTF-16 units, however wide wchar_t is: 3 + 41 * 2 + 1 = 86 > 85, 3 + 41 * 2 = 85
  EXPECT_EQ(utf16_length(L"D:\\"), 3u);
  EXPECT_EQ(utf16_length(std::wstring(1, static_cast<wchar_t>(0x1F600))), sizeof(wchar_t) == 2 ? 1u : 2u) << "one unit where wchar_t is already UTF-16";
  if (sizeof(wchar_t) > 2) {
    EXPECT_EQ(look(L"D:\\" + std::wstring(41, static_cast<wchar_t>(0x1F600)) + L"x").problem, error::too_long);
    EXPECT_EQ(look(L"D:\\" + std::wstring(41, static_cast<wchar_t>(0x1F600))).problem, error::none);
  }
}

TEST(InstallerDirCheck, aNewFolderOnAGoodDiskIsAccepted) {
  const auto v = check(L"d:/Games/串流 基地", good_disk());
  EXPECT_EQ(v.problem, error::none);
  EXPECT_EQ(v.path, L"d:\\Games\\串流 基地");
}

TEST(InstallerDirCheck, onlyFixedLocalDisks) {
  facts f = good_disk();
  f.fixed_disk = false;  // network, removable, optical, or a subst drive
  EXPECT_EQ(of(L"E:\\Sunshine", f), error::not_fixed);
  EXPECT_EQ(of(L"\\\\nas\\apps\\Sunshine"), error::not_fixed) << "UNC without asking the machine";
}

TEST(InstallerDirCheck, onlyFileSystemsWithPermissions) {
  facts f = good_disk();
  for (const wchar_t *name : { L"FAT32", L"FAT", L"exFAT", L"UDF", L"CDFS", L"" }) {
    f.file_system = name;
    EXPECT_EQ(of(L"E:\\Sunshine", f), error::fs);
  }
  for (const wchar_t *name : { L"NTFS", L"ntfs", L"ReFS", L"REFS" }) {
    f.file_system = name;
    EXPECT_EQ(of(L"E:\\Sunshine", f), error::none) << std::string(name, name + wcslen(name));
  }
}

TEST(InstallerDirCheck, notInsideWindowsOrASyncedFolder) {
  EXPECT_EQ(of(L"C:\\Windows\\Sunshine"), error::system);
  EXPECT_EQ(of(L"c:\\WINDOWS\\System32\\Sunshine"), error::system) << "case does not matter";
  EXPECT_EQ(of(L"C:\\Windowsx\\Sunshine"), error::none) << "a part, not a prefix of text";
  EXPECT_EQ(of(L"C:\\Users\\someone\\OneDrive\\Documents\\Sunshine"), error::synced) << "a synced folder is named before the user folder it sits in";
  EXPECT_EQ(of(L"C:\\Users\\someone\\OneDrive\\Sunshine"), error::synced);
  EXPECT_EQ(of(L"D:\\OneDrive\\Sunshine"), error::synced) << "a part that names a sync client";
  EXPECT_EQ(of(L"D:\\OneDrive - Contoso\\Sunshine"), error::synced);
  EXPECT_EQ(of(L"D:\\Dropbox\\Sunshine"), error::synced);
  EXPECT_EQ(of(L"D:\\Google Drive\\Sunshine"), error::synced);
  EXPECT_EQ(of(L"D:\\OneDriveRepair\\Sunshine"), error::none) << "only the name, or the name followed by a space or a dash";
  EXPECT_EQ(of(L"D:\\Sunshine"), error::none);
  EXPECT_EQ(of(L"C:\\Program Files\\Sunshine"), error::none) << "the default place is fine";
}

TEST(InstallerDirWarning, theUsersFoldersAreAWarningNotAProblem) {
  for (const wchar_t *p : { L"C:\\Users\\someone\\Foundation Sunshine", L"C:\\Users\\Foundation Sunshine", L"C:\\Users\\Public\\Foundation Sunshine", L"D:\\Documents\\Foundation Sunshine" }) {
    const verdict v = check(p, good_disk());
    EXPECT_EQ(v.problem, error::none) << std::string(p, p + wcslen(p));
    EXPECT_EQ(v.warning, error::user) << "a redirected Documents folder is a user folder too";
  }
  EXPECT_TRUE(is_warning(error::user));
  EXPECT_TRUE(is_warning(error::parent));
  for (const error e : { error::invalid, error::chars, error::not_fixed, error::fs, error::root, error::system, error::synced, error::not_empty, error::too_long, error::no_space, error::no_access })
    EXPECT_FALSE(is_warning(e)) << code(e);
  EXPECT_FALSE(is_warning(error::none));
}

TEST(InstallerDirWarning, aFolderAboveItThatUsersCanReplaceIsAWarning) {
  facts f = good_disk();
  f.parent_unsafe = true;  // D:\\Games, made by a person, with the permissions its parent gave it
  EXPECT_EQ(of(L"D:\\Games\\Foundation Sunshine", f), error::none);
  EXPECT_EQ(check(L"D:\\Games\\Foundation Sunshine", f).warning, error::parent);
  EXPECT_EQ(check(L"D:\\Games\\Foundation Sunshine\\x", f).warning, error::parent);
  EXPECT_EQ(check(L"C:\\Users\\someone\\Foundation Sunshine", f).warning, error::user) << "the user folder is the more specific words";
  f.exists = true;
  f.is_directory = true;
  f.empty = true;
  EXPECT_EQ(check(L"D:\\Games\\Foundation Sunshine", f).warning, error::parent) << "an empty folder is fine, and the warning stays";
  f.empty = false;
  const verdict full = check(L"D:\\Games\\Foundation Sunshine", f);
  EXPECT_EQ(full.problem, error::not_empty) << "a problem is found behind a warning";
  EXPECT_EQ(full.warning, error::none) << "and a refused folder shows its reason only";
  f = good_disk();
  f.parent_unsafe = true;
  f.free_bytes = 1;
  const verdict low = check(L"D:\\Games\\Foundation Sunshine", f);
  EXPECT_EQ(low.problem, error::no_space);
  EXPECT_EQ(low.warning, error::none) << "a refused folder shows its reason only, the warning is cleared";
  EXPECT_EQ(of(L"C:\\Windows\\Foundation Sunshine", f), error::system) << "a place that is refused outright stays refused";
  f = good_disk();
  f.reparse_point = true;  // D:\\Apps\\Link is a junction and Sunshine is not there yet
  EXPECT_FALSE(f.exists);
  EXPECT_EQ(of(L"D:\\Apps\\Link\\Sunshine", f), error::no_access) << "a junction above a folder that does not exist yet";
  EXPECT_EQ(of(L"C:\\Windows\\x", f), error::system) << "after the place";
  EXPECT_EQ(check(L"D:\\Sunshine", good_disk()).warning, error::none) << "a folder directly on the drive has no such parent";
}

TEST(InstallerDirCheck, theFolderMustBeNewEmptyOrOurs) {
  facts f = good_disk();
  f.exists = true;
  f.is_directory = true;
  f.empty = true;
  EXPECT_EQ(of(L"D:\\Sunshine", f), error::none) << "an existing empty folder";
  f.empty = false;
  EXPECT_EQ(of(L"D:\\Sunshine", f), error::not_empty);
  f.ours = true;
  EXPECT_EQ(of(L"D:\\Sunshine", f), error::none) << "an install of ours is updated in place";
  f.ours = false;
  f.is_directory = false;
  EXPECT_EQ(of(L"D:\\Sunshine", f), error::no_access) << "a file with that name";
  f.is_directory = true;
  f.empty = true;
  f.reparse_point = true;
  EXPECT_EQ(of(L"D:\\Sunshine", f), error::no_access) << "a junction or link would put the program somewhere else";
  f.reparse_point = false;
  f.accessible = false;
  EXPECT_EQ(of(L"D:\\Sunshine", f), error::no_access);
}

TEST(InstallerDirCheck, theDriveMustHaveRoom) {
  facts f = good_disk();
  f.free_bytes = kNeededBytes - 1;
  EXPECT_EQ(of(L"D:\\Sunshine", f), error::no_space);
  f.free_bytes = kNeededBytes;
  EXPECT_EQ(of(L"D:\\Sunshine", f), error::none);
  f.free_bytes.reset();
  EXPECT_EQ(of(L"D:\\Sunshine", f), error::none) << "room that cannot be read does not stop the install: the engine checks it again";
  EXPECT_EQ(check(L"D:\\Sunshine", good_disk(), 600ull * 1024 * 1024 * 1024).problem, error::no_space) << "the need is a parameter";
}

TEST(InstallerDirCheck, theFirstProblemAPersonCanActOnIsTheOneReported) {
  facts f = good_disk();
  f.fixed_disk = false;
  f.file_system = L"FAT32";
  EXPECT_EQ(of(L"C:\\Windows\\x", f), error::not_fixed) << "the disk before the place";
  f.fixed_disk = true;
  EXPECT_EQ(of(L"C:\\Windows\\x", f), error::fs);
  f.file_system = L"NTFS";
  EXPECT_EQ(of(L"C:\\Windows\\x", f), error::system);
  EXPECT_EQ(of(L"D:\\a!b", f), error::chars) << "the text before the machine";
  f.exists = true;
  f.is_directory = true;
  f.empty = false;
  f.free_bytes = 1;
  EXPECT_EQ(of(L"D:\\Sunshine", f), error::not_empty) << "the folder before the room";
}

TEST(InstallerDirCheck, everyErrorHasItsWord) {
  for (const error e : { error::invalid, error::chars, error::not_fixed, error::fs, error::root, error::system, error::user, error::synced, error::parent, error::not_empty, error::too_long, error::no_space, error::no_access })
    EXPECT_FALSE(code(e).empty());
  EXPECT_EQ(code(error::user), "user");
  EXPECT_TRUE(code(error::none).empty());
  EXPECT_EQ(code(error::not_empty), "not-empty");
  EXPECT_EQ(code(error::too_long), "too-long");
  EXPECT_EQ(code(error::no_space), "no-space");
  EXPECT_EQ(code(error::not_fixed), "not-fixed");
  EXPECT_EQ(code(error::parent), "parent");
}

TEST(InstallerDirInside, wholePartsOnly) {
  EXPECT_TRUE(inside(L"C:\\Windows", L"C:\\Windows"));
  EXPECT_TRUE(inside(L"C:\\Windows\\x\\y", L"C:\\Windows"));
  EXPECT_TRUE(inside(L"c:\\windows\\x", L"C:\\Windows"));
  EXPECT_FALSE(inside(L"C:\\Windows2", L"C:\\Windows"));
  EXPECT_FALSE(inside(L"C:\\Win", L"C:\\Windows"));
  EXPECT_TRUE(inside(L"C:\\x", L"C:\\"));
  EXPECT_FALSE(inside(L"D:\\x", L"C:\\"));
  EXPECT_FALSE(inside(L"C:\\x", L""));
}

TEST(InstallerDirCommandLine, theValueOfAnOptionKeepsItsSpaces) {
  EXPECT_EQ(option_value(L"--worker --run-id 1a-22 --dir \"D:\\My Games\\Sunshine\" --tasks none", L"--dir"), L"D:\\My Games\\Sunshine");
  EXPECT_EQ(option_value(L"--worker --dir D:\\Sunshine --tasks none", L"--dir"), L"D:\\Sunshine");
  EXPECT_EQ(option_value(L"--worker --dir \"D:\\串流\\基地\"", L"--dir"), L"D:\\串流\\基地");
  EXPECT_EQ(option_value(L"--worker --run-id 5", L"--dir"), L"");
  EXPECT_EQ(option_value(L"--worker --dir", L"--dir"), L"") << "an option without a value";
  EXPECT_EQ(option_value(L"--worker --directory D:\\x", L"--dir"), L"") << "the whole option name";
}

TEST(InstallerDirCommandLine, theEngineSwitchInEitherSpelling) {
  EXPECT_EQ(*dir_switch(L"Setup.exe /VERYSILENT /DIR=\"D:\\My Games\\Sunshine\" /NORESTART"), L"D:\\My Games\\Sunshine");
  EXPECT_EQ(*dir_switch(L"Setup.exe \"/DIR=D:\\My Games\\Sunshine\" /SILENT"), L"D:\\My Games\\Sunshine");
  EXPECT_EQ(*dir_switch(L"Setup.exe /SILENT /DIR=D:\\Sunshine"), L"D:\\Sunshine");
  EXPECT_EQ(*dir_switch(L"Setup.exe /silent /dir=D:\\Sunshine"), L"D:\\Sunshine") << "the switch is not case sensitive";
  EXPECT_EQ(*dir_switch(L"Setup.exe /SILENT /DIR=\"D:\\Sunshine\\\""), L"D:\\Sunshine\\") << "a path that ends in a backslash does not swallow the quote";
  EXPECT_EQ(*dir_switch(L"Setup.exe /SILENT /DIR=\"D:\\串流 基地\""), L"D:\\串流 基地");
  EXPECT_FALSE(dir_switch(L"Setup.exe /VERYSILENT /NORESTART").has_value());
  EXPECT_FALSE(dir_switch(L"Setup.exe /LOG=\"C:\\x\\DIR=y\"").has_value()) << "inside another value is not the switch";
  EXPECT_FALSE(dir_switch(L"Setup.exe /DIRX=D:\\x").has_value());
}

TEST(InstallerDirCommandLine, theEngineGetsTheCheckedPathQuoted) {
  EXPECT_EQ(engine_switch(L"D:\\My Games\\串流"), L" /DIR=\"D:\\My Games\\串流\"");
  // what comes out of look() never holds a quote, so the quoted form cannot be left early
  EXPECT_EQ(look(L"D:\\a\" /VERYSILENT /x=\"").problem, error::invalid);
}

TEST(InstallerDirPick, theProductFolderIsAppendedToWhatWasPicked) {
  const std::wstring def = L"C:\\Program Files\\Sunshine";
  auto r = from_pick(L"D:\\Games", false, false, def);
  EXPECT_EQ(r.path, L"D:\\Games\\Foundation Sunshine");
  EXPECT_TRUE(r.appended);
  EXPECT_FALSE(r.redirected);
  r = from_pick(L"D:\\", false, false, def);
  EXPECT_EQ(r.path, L"D:\\Foundation Sunshine") << "the root of a disk is never the install folder";
  EXPECT_TRUE(r.appended);
  r = from_pick(L"d:/Games/串流/", false, false, def);
  EXPECT_EQ(r.path, L"d:\\Games\\串流\\Foundation Sunshine") << "slashes, a trailing backslash and Chinese names";
  r = from_pick(L"D:\\Games", false, true, def);
  EXPECT_EQ(r.path, L"D:\\Games\\Foundation Sunshine");
  EXPECT_TRUE(r.redirected) << "a picked folder with files in it: the person is told that a folder inside it is used";
}

TEST(InstallerDirPick, nothingIsAppendedWhenThePickedFolderIsTheOneAlready) {
  const std::wstring def = L"C:\\Program Files\\Sunshine";
  auto r = from_pick(L"D:\\Games\\Foundation Sunshine", false, false, def);
  EXPECT_EQ(r.path, L"D:\\Games\\Foundation Sunshine");
  EXPECT_FALSE(r.appended);
  r = from_pick(L"D:\\Games\\foundation sunshine\\", false, false, def);
  EXPECT_EQ(r.path, L"D:\\Games\\foundation sunshine") << "the name is compared without case";
  EXPECT_FALSE(r.appended);
  r = from_pick(L"E:\\Old Sunshine", true, true, def);
  EXPECT_EQ(r.path, L"E:\\Old Sunshine") << "an install of ours is updated where it is, whatever it is called";
  EXPECT_FALSE(r.appended);
  EXPECT_FALSE(r.redirected);
  r = from_pick(L"c:\\program files\\sunshine", false, false, def);
  EXPECT_EQ(r.path, L"c:\\program files\\sunshine") << "the default folder picked again is the default folder";
  EXPECT_FALSE(r.appended);
  r = from_pick(L"D:\\Foundation Sunshine Tools", false, false, def);
  EXPECT_EQ(r.path, L"D:\\Foundation Sunshine Tools\\Foundation Sunshine") << "a part that only starts with the name is another folder";
  EXPECT_EQ(from_pick(L"", false, false, def).path, L"");
}

TEST(InstallerDirPick, thePickedAndAppendedFolderGoesThroughTheSameRules) {
  const std::wstring def = L"C:\\Program Files\\Sunshine";
  EXPECT_EQ(of(from_pick(L"D:\\", false, false, def).path), error::none);
  EXPECT_EQ(of(from_pick(L"C:\\Windows", false, false, def).path), error::system) << "inside the Windows folder is refused after the name is added too";
  EXPECT_EQ(of(from_pick(L"\\\\nas\\share", false, false, def).path), error::not_fixed);
  EXPECT_EQ(of(from_pick(L"D:\\OneDrive", false, false, def).path), error::synced);
  EXPECT_EQ(of(L"D:\\Games"), error::none) << "typed text is the folder as typed: it stays what it is";
  // the picked folder taken as it is (the "use the selected folder" choice) keeps the rule about files in it
  facts f = good_disk();
  f.exists = true;
  f.is_directory = true;
  f.empty = false;
  EXPECT_EQ(of(L"D:\\Games", f), error::not_empty);
}

TEST(InstallerDirSafe, theSafePlaceIsDirectlyOnTheDriveOrTheDefault) {
  const std::wstring def = L"C:\\Program Files\\Sunshine";
  EXPECT_EQ(safe_alternative(L"D:\\Games\\Foundation Sunshine", def), L"D:\\Foundation Sunshine");
  EXPECT_EQ(safe_alternative(L"e:\\a\\b\\c", def), L"E:\\Foundation Sunshine");
  EXPECT_EQ(safe_alternative(L"C:\\Users\\someone\\Foundation Sunshine", def), def) << "on the system drive the default place is the safe one";
  EXPECT_EQ(safe_alternative(L"c:\\Games\\x", def), def);
  EXPECT_EQ(safe_alternative(L"", def), def);
  EXPECT_EQ(of(safe_alternative(L"D:\\Games\\Foundation Sunshine", def)), error::none);
  EXPECT_EQ(check(safe_alternative(L"D:\\Games\\Foundation Sunshine", def), good_disk()).warning, error::none) << "directly on the drive: nothing above it";
}

TEST(InstallerDirCommandLine, theSilentSwitchThatAllowsAWarning) {
  EXPECT_TRUE(has_switch(L"Setup.exe /VERYSILENT /DIR=\"D:\\x\" /ALLOWUNSAFEPARENT", L"/ALLOWUNSAFEPARENT"));
  EXPECT_TRUE(has_switch(L"Setup.exe /VERYSILENT /allowunsafeparent", L"/ALLOWUNSAFEPARENT")) << "any case";
  EXPECT_FALSE(has_switch(L"Setup.exe /VERYSILENT /ALLOWUNSAFEPARENTS", L"/ALLOWUNSAFEPARENT")) << "the whole argument";
  EXPECT_FALSE(has_switch(L"Setup.exe /LOG=\"C:\\x /ALLOWUNSAFEPARENT\"", L"/ALLOWUNSAFEPARENT")) << "inside another value is not the switch";
  EXPECT_FALSE(has_switch(L"Setup.exe /VERYSILENT", L"/ALLOWUNSAFEPARENT"));
  EXPECT_EQ(kConfirmSeconds, 5);
}

TEST(InstallerDirJson, aTypedPathComesBackAsTyped) {
  // the setup UI writes {"action":{"path":"...","type":"set-dir"},"type":"action"}: a path with backslashes, quotes, Chinese and a character above U+FFFF
  EXPECT_EQ(installer_json::string_value(R"({"action":{"path":"D:\\Games\\Foundation Sunshine","type":"set-dir"},"type":"action"})", "path"), "D:\\Games\\Foundation Sunshine");
  EXPECT_EQ(installer_json::string_value(R"({"action":{"path":"D:\\串流\\基地","type":"set-dir"}})", "path"), "D:\\串流\\基地") << "raw UTF-8 passes";
  EXPECT_EQ(installer_json::string_value(R"({"action":{"path":"D:\\\u4e32\u6d41"}})", "path"), "D:\\串流") << "\\u escapes are decoded";
  EXPECT_EQ(installer_json::string_value(R"({"action":{"path":"D:\\\ud83d\ude00"}})", "path"), "D:\\\xF0\x9F\x98\x80") << "a surrogate pair is one character";
  EXPECT_EQ(installer_json::string_value(R"({"action":{"path":"D:\\a\tb\"c"}})", "path"), "D:\\a\tb\"c") << "control characters and quotes stay in the text, so that the rules refuse them";
  EXPECT_EQ(installer_json::string_value(R"({"action":{"path":"D:\\a\ud800b"}})", "path"), "D:\\a?b") << "a surrogate with no partner becomes a '?', which the rules refuse, never a replacement character";
  EXPECT_EQ(look(L"D:\\a?b").problem, error::invalid);
  EXPECT_EQ(installer_json::string_value(R"({"action":{"type":"set-dir"}})", "path"), "");
  EXPECT_EQ(installer_json::string_value(R"({"action":{"path":""}})", "path"), "");
}

TEST(InstallerDirTidy, whatWasPastedIsCleanedNotChanged) {
  EXPECT_EQ(tidy(L"D:\\Games\\Foundation Sunshine"), L"D:\\Games\\Foundation Sunshine");
  EXPECT_EQ(tidy(L"  D:\\Games  "), L"D:\\Games") << "spaces at the ends";
  EXPECT_EQ(tidy(L"\t\"D:\\串流 基地\"\t"), L"D:\\串流 基地") << "Copy as path puts quotes around it; the spaces inside stay";
  EXPECT_EQ(tidy(L"\" D:\\x \""), L"D:\\x");
  EXPECT_EQ(tidy(L"D:\\a\"b"), L"D:\\a\"b") << "a quote inside is not taken away: the rules refuse it";
  EXPECT_EQ(tidy(L"\"D:\\x"), L"\"D:\\x") << "one quote is not a pair";
  EXPECT_EQ(tidy(L"   "), L"");
  EXPECT_EQ(tidy(L"\"\""), L"");
  EXPECT_EQ(of(tidy(L"\"D:\\Games\"")), error::none);
}
