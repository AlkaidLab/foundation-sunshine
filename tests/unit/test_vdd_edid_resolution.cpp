#include "src/display_device/vdd_monitor_command.h"

#include <iostream>
#include <iterator>
#include <stdexcept>

/** @brief Exercise native-size validation, retained selections and exact create-command serialization. */
int
main() {
  using namespace display_device::vdd_edid;
  struct selection_case {
    const char *value;
    const char *resolutions;
    const char *fps;
    bool expected;
  };
  const selection_case cases[] = {
    { "default", "[]", "[]", true },
    { "3440x1440", "[1920x1080,3440x1440,3840x2160]", "[60,90,120]", true },
    { "2037x1109", "[2037X1109]", "[59.94]", true },
    { "3840x2160", "[3840x2160]", "[60]", true },
    { "3840x2160", "[3840x2160]", "[120,60]", true },
    { "4095x4095", "[4095x4095]", "[30]", true },
    { "3440x1440", "[1920x1080,3840x2160]", "[60]", false },
    { "3840x2160", "[3840x2160]", "[120,144]", false },
    { "3840x2160", "[3840x2160]", "[90]", false },
    { "5120x1440", "[5120x1440]", "[60]", false },
    { "4096x2160", "[4096x2160]", "[60]", false },
    { "0x1080", "[0x1080]", "[60]", false },
    { "1x1", "[1x1]", "[1]", false },
    { "3440x1440", "[3440x1440]", "[bad,0,481,nan,inf]", false },
    { "999999999999999999999999x1440", "[]", "[60]", false },
    { "3440x1440junk", "[3440x1440]", "[60]", false },
    { "3440x1440</EdidNativeResolution>", "[3440x1440]", "[60]", false },
    { "", "[1920x1080]", "[60]", false },
    { "1920x1080", "[]", "[60]", false },
  };
  try {
    for (const auto &test : cases) {
      if (supported_selection(test.value, test.resolutions, test.fps) != test.expected) {
        throw std::runtime_error(std::string("Incorrect EDID validation for ") + test.value);
      }
    }
    const std::map<std::string, std::string> existing {
      { "vdd_edid_native_resolution", "3840x2160" },
      { "resolutions", "[1920x1080,3840x2160]" }, { "fps", "[60,120]" }
    };
    unsigned retained_cases = 0;
    auto check_retained = [&](const std::map<std::string, std::string> &incoming,
                            const std::map<std::string, std::string> &saved, bool expected) {
      ++retained_cases;
      if (retained_selection_supported(incoming, saved) != expected)
        throw std::runtime_error("Incorrect retained EDID validation, case " + std::to_string(retained_cases));
    };
    check_retained({}, existing, true);
    check_retained({ { "sunshine_name", "legacy" } }, existing, true);
    check_retained({ { "resolutions", "[1920x1080,3840x2160]" } }, existing, true);
    check_retained({ { "resolutions", "[2560x1440,3840x2160]" } }, existing, true);
    check_retained({ { "resolutions", "[1920x1080]" } }, existing, false);
    check_retained({ { "resolutions", "[]" } }, existing, false);
    check_retained({ { "fps", "[120,144]" } }, existing, false);
    check_retained({ { "fps", "[]" } }, existing, false);
    check_retained({ { "fps", "[120,60]" } }, existing, true);
    check_retained({ { "resolutions", "[1920x1080]" }, { "vdd_edid_native_resolution", "default" } }, existing, true);
    check_retained({ { "resolutions", "[]" } }, { { "vdd_edid_native_resolution", "default" } }, true);
    check_retained({ { "resolutions", "[]" } }, {}, true);
    const std::string guid = "{00000001-0002-0003-0405-060708090a0b}";
    unsigned command_cases = 0;
    auto check_command = [&](std::string_view id, float w, float h, std::string_view size, const std::string &expected) {
      ++command_cases;
      if (create_monitor_command(id, 1000, 0.0001f, 1000, w, h, size) != expected)
        throw std::runtime_error("Incorrect create command, case " + std::to_string(command_cases));
    };
    check_command(guid, 0, 0, "default", "CREATEMONITOR " + guid + ":[1000,0.0001,1000]");
    check_command(guid, 34.5f, 19.4f, "default", "CREATEMONITOR " + guid + ":[1000,0.0001,1000][34.5,19.4]");
    check_command(guid, 0, 0, "3840x2160", "CREATEMONITOR " + guid + ":[1000,0.0001,1000][0,0][edid=3840x2160]");
    check_command(guid, 34.5f, 19.4f, "2736x1824", "CREATEMONITOR " + guid + ":[1000,0.0001,1000][34.5,19.4][edid=2736x1824]");
    check_command("", 0, 0, "default", "CREATEMONITOR");
    check_command("", 0, 0, "3840x2160", "CREATEMONITOR :[1000,0.0001,1000][0,0][edid=3840x2160]");
    check_command(guid, 34.5f, 0, "3840x2160", "CREATEMONITOR " + guid + ":[1000,0.0001,1000][0,0][edid=3840x2160]");
    for (const auto invalid : { "", "4096x2160", "3840x2160][999,999]", "3840X2160" }) {
      check_command(guid, 0, 0, invalid, "CREATEMONITOR " + guid + ":[1000,0.0001,1000]");
    }
    std::cout << "PASS: " << command_cases << " create-command cases\n";
    std::cout << "PASS: " << std::size(cases) << " EDID selection cases and " << retained_cases << " legacy mode-list update cases\n";
    return 0;
  }
  catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
