#!/usr/bin/env python3
"""Execute production gamepad retirement/timer code with a deterministic platform.

Run: python3 tests/tools/controller_handoff_probe.py
No Windows driver or Sunshine dependency build is needed. The fixture uses the
actual struct and controller packet handler from input.cpp, not copied logic.
"""
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
source = (ROOT / "src/input.cpp").read_text()
slot = source[source.index("  struct gamepad_t {"):source.index("  struct modifier_state_t {")]
start = source.index("  void\n  passthrough(std::shared_ptr<input_t> &input, PNV_MULTI_CONTROLLER_PACKET packet)")
handler = source[start:source.index("  enum class batch_result_e", start)]

fixture = r'''
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>
using namespace std::chrono_literals;
using namespace std::literals;
struct job_t { std::function<void()> action; bool cancelled = false; };
namespace thread_pool_util { struct ThreadPool { using task_id_t = job_t*; }; }
struct scheduler_t {
  std::vector<std::unique_ptr<job_t>> jobs;
  struct result_t { job_t* task_id; };
  template<class F, class D> result_t pushDelayed(F action, D) {
    jobs.push_back(std::make_unique<job_t>(job_t{std::move(action)}));
    return {jobs.back().get()};
  }
  void cancel(job_t* job) { if (job) job->cancelled = true; }
  template<class F> void push(F action) { action(); }
  void run(job_t* job) { assert(job && !job->cancelled); job->action(); }
} task_pool;
namespace platf {
  constexpr std::uint32_t BACK = BACK_FLAG, HOME = SPECIAL_FLAG;
  struct gamepad_state_t {
    std::uint32_t buttonFlags {};
    unsigned char lt {}, rt {};
    short lsX {}, lsY {}, rsX {}, rsY {};
  };
  struct gamepad_id_t { int global; std::uint8_t local; };
  struct arrival_t {};
  std::vector<gamepad_state_t> writes;
  int alloc_gamepad(int&, gamepad_id_t, arrival_t, int, int) { return 0; }
  bool gamepad_is_ds5(int&, int) { return false; }
  void gamepad_update(int&, int id, gamepad_state_t state) { assert(id >= 0); writes.push_back(state); }
}
namespace config {
  struct input_config_t { bool controller = true; std::chrono::milliseconds back_button_timeout {1000}; } input;
}
namespace util { struct hex { explicit hex(int) {} std::string to_string_view() { return {}; } }; }
#define BOOST_LOG(level) std::cerr
enum class button_state_e { NONE, UP, DOWN };
int platf_input = 0, gamepadMask = 0;
int alloc_id(int& mask) { assert(mask == 0); mask = 1; return 0; }
void free_id(int& mask, int) { mask = 0; }
void free_gamepad(int&, int) { gamepadMask = 0; }
// PRODUCTION_SLOT
struct input_t { std::vector<gamepad_t> gamepads {1}; int feedback_queue {}, client_gamepad {}; };
void log_unallocated_controller(const input_t&, int) {}
// PRODUCTION_HANDLER
int main() {
  auto input = std::make_shared<input_t>();
  auto send = [&](short mask, unsigned short buttons) {
    NV_MULTI_CONTROLLER_PACKET packet {};
    packet.activeGamepadMask = mask;
    packet.buttonFlags = buttons;
    passthrough(input, &packet);
  };
  auto& slot = input->gamepads[0];
  send(1, platf::BACK);
  auto old_back = slot.back_timeout_id->action;
  const auto generation = slot.generation;
  send(0, 0);
  assert(slot.id == -1 && slot.gamepad_state.buttonFlags == 0);
  assert(slot.back_button_state == button_state_e::NONE && !slot.back_timeout_id);
  send(1, 0);
  assert(slot.generation != generation);
  auto count = platf::writes.size();
  old_back(); // Even a callback already removed from the scheduler cannot reuse the slot.
  assert(platf::writes.size() == count);

  send(1, platf::BACK);
  task_pool.run(slot.back_timeout_id);
  assert(slot.gamepad_state.buttonFlags & platf::HOME);
  auto old_home = slot.home_release_timeout_id->action;
  auto home_job = slot.home_release_timeout_id;
  send(1, 0); // Ordinary reports cannot shorten the synthesized Home press.
  assert(slot.gamepad_state.buttonFlags & platf::HOME);
  send(0, 0);
  assert(home_job->cancelled && !slot.home_release_timeout_id);
  send(1, A_FLAG);
  count = platf::writes.size();
  old_home();
  assert(platf::writes.size() == count && slot.gamepad_state.buttonFlags == A_FLAG);

  send(1, platf::BACK);
  task_pool.run(slot.back_timeout_id);
  task_pool.run(slot.home_release_timeout_id);
  assert(!(slot.gamepad_state.buttonFlags & platf::HOME));
  send(1, 0);
  send(1, platf::BACK);
  task_pool.run(slot.back_timeout_id);
  send(1, platf::HOME); // A real held Home survives the synthetic release.
  task_pool.run(slot.home_release_timeout_id);
  assert(slot.gamepad_state.buttonFlags & platf::HOME);
  send(0, 0);

  // Two Back long presses within the Home pulse must not share a release task.
  config::input.back_button_timeout = 0ms;
  send(1, platf::BACK);
  task_pool.run(slot.back_timeout_id);
  auto previous_release_job = slot.home_release_timeout_id;
  old_home = previous_release_job->action;
  send(1, 0);
  send(1, platf::BACK);
  task_pool.run(slot.back_timeout_id);
  auto latest_release_job = slot.home_release_timeout_id;
  assert(previous_release_job->cancelled && latest_release_job != previous_release_job);
  count = platf::writes.size();
  old_home(); // A callback already dequeued before cancellation must also be harmless.
  assert(platf::writes.size() == count && slot.home_release_timeout_id == latest_release_job);
  send(1, 0);
  assert(slot.gamepad_state.buttonFlags & platf::HOME);
  task_pool.run(latest_release_job);
  assert(!(slot.gamepad_state.buttonFlags & platf::HOME));
  send(0, 0);
  old_back = {}; old_home = {};
  for (auto& job : task_pool.jobs) job->action = {};
  std::cout << "Controller retirement and delayed Home regression checks passed\n";
}
'''
fixture = fixture.replace("// PRODUCTION_SLOT", slot).replace("// PRODUCTION_HANDLER", handler)
with tempfile.TemporaryDirectory(prefix="sunshine-controller-handoff-") as directory:
    path = pathlib.Path(directory)
    (path / "probe.cpp").write_text(fixture)
    compiler = shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise RuntimeError("A C++ compiler is required")
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
                    "-I" + str(ROOT / "third-party"), str(path / "probe.cpp"),
                    "-o", str(path / "probe")], check=True)
    subprocess.run([str(path / "probe")], check=True)
