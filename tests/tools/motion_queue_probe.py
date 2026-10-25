#!/usr/bin/env python3
"""Check sensor ordering and coalescing through the production input queue walker."""
import argparse
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
source = (ROOT / "src/input.cpp").read_text(encoding="utf-8")
start = source.index("  enum class batch_result_e")
end = source.index("  /**\n   * @brief Called on the control stream thread to queue an input message.", start)
production = source[start:end]

fixture = r'''
#include <bit>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
#include "src/pen_barrel_roll.h"
namespace util::endian {
  template<class T> T big(T value) { return std::byteswap(value); }
  template<class T> T little(T value) { return value; }
}
static_assert(std::endian::native == std::endian::little);
namespace config { struct { bool input_activity_boost = false; } video; }
namespace input {
  std::vector<SS_CONTROLLER_MOTION_PACKET> delivered;
  struct input_t {
    std::list<std::vector<uint8_t>> input_queue;
    std::mutex input_queue_lock;
    struct { std::optional<bool> evaluate(PNV_INPUT_HEADER, size_t) { return false; } } activity_tracker;
    struct sink { void raise(std::chrono::steady_clock::time_point) {} } activity_sink;
    sink* input_activity_event = &activity_sink;
  };
  void print(void*) {}
  template<class... T> void passthrough(T&&...) {}
  void passthrough(std::shared_ptr<input_t>&, PSS_CONTROLLER_MOTION_PACKET packet) {
    delivered.push_back(*packet);
  }
// PRODUCTION
}
int main() {
  auto input = std::make_shared<input::input_t>();
  std::vector<SS_CONTROLLER_MOTION_PACKET> packets(4);
  // G1 A1 G2 G0 must become G1 A1 G0: keep the sensor barrier and merge only G2/G0.
  for (size_t i = 0; i < packets.size(); ++i) {
    auto& packet = packets[i];
    packet.header.magic = SS_CONTROLLER_MOTION_MAGIC;
    packet.header.size = util::endian::big(uint32_t(sizeof(packet) - 4));
    packet.motionType = i == 1 ? LI_MOTION_TYPE_ACCEL : LI_MOTION_TYPE_GYRO;
    const float x = i == 3 ? 0.f : float(i + 1);
    std::memcpy(packet.x, &x, sizeof(x));
    input->input_queue.emplace_back(sizeof(packet));
    std::memcpy(input->input_queue.back().data(), &packet, sizeof(packet));
  }
  while (!input->input_queue.empty()) input::passthrough_next_message(input);
  assert(input::delivered.size() == 3);
  assert(std::memcmp(&input::delivered[0], &packets[0], sizeof(packets[0])) == 0);
  assert(std::memcmp(&input::delivered[1], &packets[1], sizeof(packets[1])) == 0);
  assert(std::memcmp(&input::delivered[2], &packets[3], sizeof(packets[3])) == 0);
}
'''
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compiler", default="c++", help="GNU/Clang compiler selected by CMake")
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="sunshine-motion-queue-") as directory:
    path = pathlib.Path(directory)
    (path / "probe.cpp").write_text(fixture.replace("// PRODUCTION", production), encoding="utf-8")
    binary = path / ("probe.exe" if os.name == "nt" else "probe")
    subprocess.run([args.compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
                    "-I" + str(ROOT), "-I" + str(ROOT / "third-party"), str(path / "probe.cpp"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
