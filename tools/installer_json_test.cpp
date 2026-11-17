// Regression test of the 2026-11-09 blocker: the web host read the nested "type" of an action line.
//   g++ -std=c++23 installer_json_test.cpp -o installer_json_test && ./installer_json_test
#include "installer_json.h"

#include <cstdio>
#include <string>

static int failures = 0;
static void
expect(const std::string &line, const char *key, const std::string &want) {
  const std::string got = installer_json::top_str(line, key);
  if (got != want) {
    std::printf("FAIL %s [%s]: got '%s', want '%s'\n", line.c_str(), key, got.c_str(), want.c_str());
    ++failures;
  }
}

int
main() {
  // what the setup UI writes (serde_json, keys in alphabetical order)
  expect(R"({"action":{"type":"ready"},"type":"action"})", "type", "action");
  expect(R"({"action":{"id":"usbip","type":"toggle-component"},"type":"action"})", "type", "action");
  expect(R"({"hwnd":394844,"type":"hello"})", "type", "hello");
  expect(R"({"text":"reveal by ready","type":"log"})", "type", "log");
  expect(R"({"text":"reveal by ready","type":"log"})", "text", "reveal by ready");
  // type first, spaces, escapes, braces and a "type" inside a string value
  expect(R"({"type":"action","action":{"type":"install"}})", "type", "action");
  expect(R"({ "type" : "loaded" })", "type", "loaded");
  expect(R"({"text":"a \"type\":\"x\" {in} text","type":"log"})", "type", "log");
  expect(R"({"text":"a \"quoted\" word","type":"log"})", "text", "a \"quoted\" word");
  expect(R"({"list":[{"type":"a"}],"type":"b"})", "type", "b");
  expect(R"({"typed":"no","type":"yes"})", "type", "yes");
  expect(R"({"action":{"type":"ready"}})", "type", "");
  expect("", "type", "");
  std::printf(failures ? "%d failure(s)\n" : "installer_json: all passed\n", failures);
  return failures ? 1 : 0;
}
