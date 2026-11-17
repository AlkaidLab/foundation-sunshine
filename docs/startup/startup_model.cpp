// A model of the ORDER of the start, not of the machine: the durations are the ones the 12:53 log of a Windows test host gives, and the waits are the real gates of src/startup_gate.h.
// It answers one question: how much sooner is each thing available when the input devices are opened beside the probe, the servers open before it, and the service tries at once.
#include <src/startup_gate.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

using namespace std::chrono;
using clk = steady_clock;

struct durations_t {
  int service_wait_ms;   // sunshinesvc: 3000 before, 0 after
  int service_spawn_ms;  // process creation up to the first log line (service 00.0 -> first line 04.4, minus the 3 s wait)
  int pre_ms;            // up to nvprefs (04.400 -> 04.561)
  int serial_ms;         // platf::init + proc::init (not parallelizable): a share of the 1489 ms gap
  int input_ms;          // input::init (HID enumeration of the virtual mouse): the rest of the gap
  int probe_ms;          // 1231
};

struct result_t { long long ports, serverinfo, stream; };

static long long since(clk::time_point t0) { return duration_cast<milliseconds>(clk::now() - t0).count(); }

static result_t before(const durations_t &d) {
  const auto t0 = clk::now();
  std::this_thread::sleep_for(milliseconds(d.service_wait_ms + d.service_spawn_ms + d.pre_ms + d.serial_ms + d.input_ms));
  std::this_thread::sleep_for(milliseconds(d.probe_ms));
  const auto ports = since(t0);  // nvhttp binds only now
  return { ports, ports, ports };  // a client that asks now is answered at once
}

static result_t after(const durations_t &d, startup::gate_t &probe, startup::gate_t &input) {
  const auto t0 = clk::now();
  std::this_thread::sleep_for(milliseconds(d.service_wait_ms + d.service_spawn_ms + d.pre_ms + d.serial_ms));
  std::thread in([&] { std::this_thread::sleep_for(milliseconds(d.input_ms)); input.open(); });
  const auto ports = since(t0);  // nvhttp binds here
  // a client asks the moment the port is open
  long long serverinfo = 0, stream = 0;
  std::thread client([&] {
    probe.wait_for(seconds(10)); serverinfo = since(t0);
    probe.wait_for(seconds(10)); input.wait_for(seconds(10)); stream = since(t0);
  });
  std::this_thread::sleep_for(milliseconds(d.probe_ms));
  probe.open();
  in.join(); client.join();
  return { ports, serverinfo, stream };
}

int main() {
  struct scenario_t { const char *name; durations_t d; } scenarios[] = {
    { "gap is all input::init (best case for concurrency)", { 3000, 1400, 161, 0, 1489, 1231 } },
    { "gap is 300 ms platf/proc + 1189 ms input::init", { 3000, 1400, 161, 300, 1189, 1231 } },
    { "gap is 1 s platf/proc + 489 ms input::init (worst case)", { 3000, 1400, 161, 1000, 489, 1231 } },
  };
  std::printf("%-58s %s\n", "scenario (times from the start of the service, ms)", "ports open | first serverinfo | stream ready");
  for (auto &s : scenarios) {
    auto b = before(s.d);
    auto d2 = s.d;
    d2.service_wait_ms = 0;
    startup::gate_t probe, input;
    auto a = after(d2, probe, input);
    std::printf("%-58s\n  before: %6lld | %6lld | %6lld\n  after : %6lld | %6lld | %6lld\n", s.name, b.ports, b.serverinfo, b.stream, a.ports, a.serverinfo, a.stream);
  }
}
