#!/usr/bin/env python3
"""Run production input batching and queue dispatch with a recording motion sink.

No driver is needed. Activity validation and unrelated platform sinks are stubbed;
the packet layouts, batching overloads and queue walker come from production code.
"""
import argparse
import os
import pathlib
import shutil
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
#include <iostream>
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
  std::vector<SS_CONTROLLER_MOTION_PACKET> expected;
  auto enqueue = [&](uint8_t type, float x, uint8_t controller = 0) {
    SS_CONTROLLER_MOTION_PACKET packet {};
    packet.header.magic = SS_CONTROLLER_MOTION_MAGIC;
    packet.header.size = util::endian::big(uint32_t(sizeof(packet) - 4));
    packet.motionType = type;
    packet.controllerNumber = controller;
    const float y = x * .5f, z = -x * .25f;
    std::memcpy(packet.x, &x, 4);
    std::memcpy(packet.y, &y, 4);
    std::memcpy(packet.z, &z, 4);
    expected.push_back(packet);
    std::vector<uint8_t> bytes(sizeof(packet));
    std::memcpy(bytes.data(), &packet, sizeof(packet));
    input->input_queue.push_back(std::move(bytes));
  };
  auto verify = [&] {
    while (!input->input_queue.empty()) input::passthrough_next_message(input);
    assert(input::delivered.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
      assert(std::memcmp(&input::delivered[i], &expected[i], sizeof(expected[i])) == 0);
    }
    input::passthrough_next_message(input);
    assert(input::delivered.size() == expected.size());
    expected.clear();
    input::delivered.clear();
  };
  // A later reading must not jump over the other sensor, including a partial tail.
  for (uint8_t first : {1, 2}) {
    enqueue(first, 1);
    enqueue(3 - first, 1);
    enqueue(first, 2);
    verify();
  }
  // Both frame orders and interleaved controllers survive realistic backlog sizes.
  // Full four-sample cycles have zero mean; byte equality preserves the source-time integral.
  for (int count : {1, 2, 4, 8, 32, 128}) {
    for (uint8_t first : {1, 2}) {
      for (int i = 0; i < count; ++i) {
        const float x = (i % 4 == 3) ? -3.f : 1.f;
        for (uint8_t controller : {0, 1}) {
          enqueue(first, x, controller);
          enqueue(3 - first, x, controller);
        }
      }
      if (first == 1) enqueue(1, 9);
      enqueue(2, 0); // A final stop and subsequent acceleration must retain their order.
      enqueue(1, 9);
      verify();
    }
  }
  // Retain existing same-sensor coalescing and its final zero; this patch changes only the barrier.
  enqueue(2, 10);
  enqueue(2, 20);
  enqueue(2, 0);
  expected.erase(expected.begin(), expected.end() - 1);
  verify();
  std::cout << "Motion queue: sensor barriers, paired backlogs, final zero and idle checks passed\n";
}
'''
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compiler", help="GNU/Clang compiler selected by the parent CMake build")
args = parser.parse_args()
compiler = args.compiler or shutil.which("clang++") or shutil.which("g++")
if not compiler:
    raise RuntimeError("A C++23 GNU/Clang compiler is required")
with tempfile.TemporaryDirectory(prefix="sunshine-motion-queue-") as directory:
    path = pathlib.Path(directory)
    (path / "probe.cpp").write_text(fixture.replace("// PRODUCTION", production), encoding="utf-8")
    binary = path / ("probe.exe" if os.name == "nt" else "probe")
    subprocess.run([compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
                    "-I" + str(ROOT), "-I" + str(ROOT / "third-party"), str(path / "probe.cpp"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
