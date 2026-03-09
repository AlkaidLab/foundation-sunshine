# macOS 鼠标专用实时线程 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Eliminate mouse stutter by moving all mouse event processing to a dedicated real-time thread, bypassing the task_pool queue entirely.

**Architecture:** Control stream thread writes mouse events to lock-free atomic state. A dedicated real-time thread (THREAD_TIME_CONSTRAINT_POLICY) reads the atomic state and calls CGEventPost. All other input (keyboard, gamepad) continues through the existing task_pool.

**Tech Stack:** C++17 atomics, macOS Mach thread policies, dispatch_semaphore_t, CGEvent API

---

### Task 1: Add atomic mouse state and thread fields to macos_input_t

**Files:**
- Modify: `src/platform/macos/input.cpp:1-60` (includes and struct definition)

**Step 1: Add required includes**

Add after the existing `#include <thread>` (line 18):

```cpp
#include <atomic>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>
```

**Step 2: Add atomic state struct and thread fields to macos_input_t**

Add inside `struct macos_input_t` after the `bool display_validated {false};` field (line 60):

```cpp
    // --- Dedicated mouse thread (real-time priority) ---
    // Atomic state: control stream thread writes, mouse thread reads
    struct alignas(64) {
      std::atomic<float> abs_x {0};
      std::atomic<float> abs_y {0};
      std::atomic<float> abs_prev_x {0};
      std::atomic<float> abs_prev_y {0};
      std::atomic<int> rel_dx {0};
      std::atomic<int> rel_dy {0};
      std::atomic<uint32_t> move_seq {0};
      std::atomic<bool> is_absolute {true};

      // Button events use a small ring buffer (max 8 pending)
      struct button_event_t {
        int button;
        bool release;
      };
      static constexpr int BUTTON_RING_SIZE = 8;
      button_event_t button_ring[BUTTON_RING_SIZE] {};
      std::atomic<uint32_t> button_write_idx {0};
      std::atomic<uint32_t> button_read_idx {0};
    } mouse_state;

    dispatch_semaphore_t mouse_semaphore {};
    std::thread mouse_thread;
    std::atomic<bool> mouse_thread_running {false};

    // Mouse thread owns these — never touched by other threads
    CGEventSourceRef mt_source {};
    CGEventRef mt_event {};
    float mt_pos_x {0};
    float mt_pos_y {0};
```

**Step 3: Build to verify struct compiles**

Run: `ninja -C build 2>&1 | tail -5`
Expected: Compiles with 0 errors (warnings OK)

**Step 4: Commit**

```bash
git add src/platform/macos/input.cpp
git commit -m "feat(macos/input): add atomic mouse state and thread fields to macos_input_t"
```

---

### Task 2: Implement mouse thread main loop and CGEvent posting

**Files:**
- Modify: `src/platform/macos/input.cpp` (add functions before `input()`)

**Step 1: Add mouse thread helper functions**

Add these functions right before the `input()` function (before line 653 `input_t input()`). Place them after the `hscroll` function:

```cpp
  // --- Mouse thread functions (run on dedicated real-time thread) ---

  CGEventType
  mt_event_type(const bool mouse_down[3]) {
    if (mouse_down[0]) return kCGEventLeftMouseDragged;
    if (mouse_down[1]) return kCGEventOtherMouseDragged;
    if (mouse_down[2]) return kCGEventRightMouseDragged;
    return kCGEventMouseMoved;
  }

  void
  mt_post_event(macos_input_t *mi, CGEventType type, CGMouseButton button,
                float x, float y, float prev_x, float prev_y, int click_count) {
    const auto &bounds = mi->cached_display_bounds;
    CGPoint loc {
      std::clamp((double) x, bounds.origin.x, bounds.origin.x + bounds.size.width - 1),
      std::clamp((double) y, bounds.origin.y, bounds.origin.y + bounds.size.height - 1)
    };

    CGEventRef ev = mi->mt_event;
    CGEventSetType(ev, type);
    CGEventSetLocation(ev, loc);
    CGEventSetIntegerValueField(ev, kCGMouseEventButtonNumber, button);
    CGEventSetIntegerValueField(ev, kCGMouseEventClickState, click_count);
    CGEventSetDoubleValueField(ev, kCGMouseEventDeltaX, x - prev_x);
    CGEventSetDoubleValueField(ev, kCGMouseEventDeltaY, y - prev_y);
    CGEventPost(kCGHIDEventTap, ev);

    mi->mt_pos_x = loc.x;
    mi->mt_pos_y = loc.y;
  }

  void
  mouse_thread_func(macos_input_t *mi) {
    // Set real-time thread scheduling policy
    thread_time_constraint_policy_data_t policy;
    policy.period = 1000000;       // 1ms in abs time
    policy.computation = 250000;   // 0.25ms
    policy.constraint = 500000;    // 0.5ms
    policy.preemptible = true;
    thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t) &policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);

    BOOST_LOG(info) << "Mouse thread started (real-time priority)";

    bool mouse_down[3] {};
    uint32_t last_move_seq = 0;

    while (mi->mouse_thread_running.load(std::memory_order_relaxed)) {
      // Wait for signal with 2ms timeout (fallback polling)
      dispatch_semaphore_wait(mi->mouse_semaphore,
                              dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_MSEC));

      // Process all pending button events first (preserves ordering)
      uint32_t br = mi->mouse_state.button_read_idx.load(std::memory_order_relaxed);
      uint32_t bw = mi->mouse_state.button_write_idx.load(std::memory_order_acquire);
      while (br != bw) {
        auto &evt = mi->mouse_state.button_ring[br % mi->mouse_state.BUTTON_RING_SIZE];
        int btn = evt.button;
        bool release = evt.release;

        CGMouseButton mac_button;
        CGEventType etype;
        int btn_idx = 0;
        switch (btn) {
          case 1:
            mac_button = kCGMouseButtonLeft;
            etype = release ? kCGEventLeftMouseUp : kCGEventLeftMouseDown;
            btn_idx = 0;
            break;
          case 2:
            mac_button = kCGMouseButtonCenter;
            etype = release ? kCGEventOtherMouseUp : kCGEventOtherMouseDown;
            btn_idx = 1;
            break;
          case 3:
            mac_button = kCGMouseButtonRight;
            etype = release ? kCGEventRightMouseUp : kCGEventRightMouseDown;
            btn_idx = 2;
            break;
          default:
            br++;
            continue;
        }
        mouse_down[btn_idx] = !release;
        mt_post_event(mi, etype, mac_button, mi->mt_pos_x, mi->mt_pos_y,
                      mi->mt_pos_x, mi->mt_pos_y, 1);

        br++;
      }
      mi->mouse_state.button_read_idx.store(br, std::memory_order_release);

      // Process move event
      uint32_t seq = mi->mouse_state.move_seq.load(std::memory_order_acquire);
      if (seq == last_move_seq) continue;
      last_move_seq = seq;

      CGEventType move_type = mt_event_type(mouse_down);

      if (mi->mouse_state.is_absolute.load(std::memory_order_relaxed)) {
        float ax = mi->mouse_state.abs_x.load(std::memory_order_relaxed);
        float ay = mi->mouse_state.abs_y.load(std::memory_order_relaxed);
        float px = mi->mouse_state.abs_prev_x.load(std::memory_order_relaxed);
        float py = mi->mouse_state.abs_prev_y.load(std::memory_order_relaxed);
        mt_post_event(mi, move_type, kCGMouseButtonLeft, ax, ay, px, py, 0);
      }
      else {
        // Relative: atomically consume accumulated deltas
        int dx = mi->mouse_state.rel_dx.exchange(0, std::memory_order_relaxed);
        int dy = mi->mouse_state.rel_dy.exchange(0, std::memory_order_relaxed);
        if (dx != 0 || dy != 0) {
          float nx = mi->mt_pos_x + dx * mi->mouse_sensitivity;
          float ny = mi->mt_pos_y + dy * mi->mouse_sensitivity;
          mt_post_event(mi, move_type, kCGMouseButtonLeft, nx, ny,
                        mi->mt_pos_x, mi->mt_pos_y, 0);
        }
      }
    }

    BOOST_LOG(info) << "Mouse thread stopped";
  }
```

**Step 2: Build to verify**

Run: `ninja -C build 2>&1 | tail -5`
Expected: Compiles (functions are defined but not yet called)

**Step 3: Commit**

```bash
git add src/platform/macos/input.cpp
git commit -m "feat(macos/input): implement mouse thread main loop with real-time scheduling"
```

---

### Task 3: Start/stop mouse thread in input() and freeInput()

**Files:**
- Modify: `src/platform/macos/input.cpp` — `input()` function (~line 653) and `freeInput()` (~line 722)

**Step 1: Initialize and start mouse thread in input()**

After the existing `macos_input->position_cache_valid = false;` line (around line 713), add:

```cpp
    // Create dedicated mouse thread with its own CGEvent resources
    macos_input->mouse_semaphore = dispatch_semaphore_create(0);
    macos_input->mt_source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    CGEventSourceSetLocalEventsSuppressionInterval(macos_input->mt_source, 0.0);
    macos_input->mt_event = CGEventCreateMouseEvent(macos_input->mt_source, kCGEventMouseMoved, CGPointZero, kCGMouseButtonLeft);
    macos_input->mouse_thread_running.store(true, std::memory_order_relaxed);
    macos_input->mouse_thread = std::thread(mouse_thread_func, macos_input);
```

**Step 2: Stop mouse thread and release resources in freeInput()**

Replace the existing `freeInput` function with:

```cpp
  void
  freeInput(void *p) {
    auto *input = static_cast<macos_input_t *>(p);

    // Stop mouse thread
    if (input->mouse_thread_running.load()) {
      input->mouse_thread_running.store(false, std::memory_order_relaxed);
      dispatch_semaphore_signal(input->mouse_semaphore);
      if (input->mouse_thread.joinable()) {
        input->mouse_thread.join();
      }
    }

    // Release mouse thread resources
    if (input->mt_event) CFRelease(input->mt_event);
    if (input->mt_source) CFRelease(input->mt_source);
    if (input->mouse_semaphore) dispatch_release(input->mouse_semaphore);

    CFRelease(input->source);
    CFRelease(input->kb_event);
    CFRelease(input->mouse_event);

    delete input;
  }
```

**Step 3: Build and verify**

Run: `ninja -C build 2>&1 | tail -5`
Expected: Compiles with 0 errors

**Step 4: Commit**

```bash
git add src/platform/macos/input.cpp
git commit -m "feat(macos/input): start/stop mouse thread in input lifecycle"
```

---

### Task 4: Add platform fast-path API for mouse events

**Files:**
- Modify: `src/platform/common.h:860-870` — add new function declarations
- Modify: `src/platform/macos/input.cpp` — implement fast-path functions

**Step 1: Declare fast-path functions in common.h**

Add after the existing `button_mouse` declaration (after line 870):

```cpp
  // Fast-path mouse functions — write to atomic state, bypass task_pool
  // Only implemented on macOS; other platforms fall back to regular functions
  void
  mouse_move_fast(input_t &input, const touch_port_t &touch_port, float x, float y);
  void
  mouse_move_rel_fast(input_t &input, int deltaX, int deltaY);
  void
  mouse_button_fast(input_t &input, int button, bool release);
```

**Step 2: Implement fast-path functions in macos/input.cpp**

Add these after the existing `button_mouse` function (after the `scroll` function area, before `hscroll`):

```cpp
  void
  mouse_move_fast(input_t &input, const touch_port_t &touch_port, float x, float y) {
    const auto mi = static_cast<macos_input_t *>(input.get());

    refresh_display_if_needed(mi);

    const auto scaling = mi->displayScaling;
    const auto &bounds = mi->cached_display_bounds;
    float ax = x * scaling + bounds.origin.x;
    float ay = y * scaling + bounds.origin.y;

    if (!mi->abs_mouse_initialized) {
      CGAssociateMouseAndMouseCursorPosition(false);
      CGWarpMouseCursorPosition(CGPoint { ax, ay });
      CGAssociateMouseAndMouseCursorPosition(true);
      mi->abs_mouse_initialized = true;
      mi->mt_pos_x = ax;
      mi->mt_pos_y = ay;
      BOOST_LOG(info) << "mouse_move_fast: initial warp to (" << ax << ", " << ay << ")";
    }

    float prev_x = mi->mouse_state.abs_x.load(std::memory_order_relaxed);
    float prev_y = mi->mouse_state.abs_y.load(std::memory_order_relaxed);
    mi->mouse_state.abs_prev_x.store(prev_x, std::memory_order_relaxed);
    mi->mouse_state.abs_prev_y.store(prev_y, std::memory_order_relaxed);
    mi->mouse_state.abs_x.store(ax, std::memory_order_relaxed);
    mi->mouse_state.abs_y.store(ay, std::memory_order_relaxed);
    mi->mouse_state.is_absolute.store(true, std::memory_order_relaxed);
    mi->mouse_state.move_seq.fetch_add(1, std::memory_order_release);
    dispatch_semaphore_signal(mi->mouse_semaphore);
  }

  void
  mouse_move_rel_fast(input_t &input, int deltaX, int deltaY) {
    const auto mi = static_cast<macos_input_t *>(input.get());

    mi->mouse_state.rel_dx.fetch_add(deltaX, std::memory_order_relaxed);
    mi->mouse_state.rel_dy.fetch_add(deltaY, std::memory_order_relaxed);
    mi->mouse_state.is_absolute.store(false, std::memory_order_relaxed);
    mi->mouse_state.move_seq.fetch_add(1, std::memory_order_release);
    dispatch_semaphore_signal(mi->mouse_semaphore);
  }

  void
  mouse_button_fast(input_t &input, int button, bool release) {
    const auto mi = static_cast<macos_input_t *>(input.get());

    uint32_t wi = mi->mouse_state.button_write_idx.load(std::memory_order_relaxed);
    uint32_t ri = mi->mouse_state.button_read_idx.load(std::memory_order_relaxed);
    // Drop if ring full (shouldn't happen — 8 slots is plenty)
    if (wi - ri >= mi->mouse_state.BUTTON_RING_SIZE) return;

    auto &slot = mi->mouse_state.button_ring[wi % mi->mouse_state.BUTTON_RING_SIZE];
    slot.button = button;
    slot.release = release;
    mi->mouse_state.button_write_idx.store(wi + 1, std::memory_order_release);
    dispatch_semaphore_signal(mi->mouse_semaphore);
  }
```

**Step 3: Add stub implementations for non-macOS platforms**

In `src/platform/linux/input/inputtino.cpp` and `src/platform/windows/input.cpp`, add stubs (search for existing `button_mouse` and add after):

```cpp
  void mouse_move_fast(input_t &input, const touch_port_t &touch_port, float x, float y) {
    abs_mouse(input, touch_port, x, y);  // fallback
  }
  void mouse_move_rel_fast(input_t &input, int deltaX, int deltaY) {
    move_mouse(input, deltaX, deltaY);  // fallback
  }
  void mouse_button_fast(input_t &input, int button, bool release) {
    button_mouse(input, button, release);  // fallback
  }
```

**Step 4: Build and verify**

Run: `ninja -C build 2>&1 | tail -5`
Expected: Compiles with 0 errors

**Step 5: Commit**

```bash
git add src/platform/common.h src/platform/macos/input.cpp
git commit -m "feat(input): add fast-path mouse API for lock-free dispatch"
```

---

### Task 5: Wire control stream to fast-path, bypass task_pool for mouse

**Files:**
- Modify: `src/input.cpp:1656-1668` — the `passthrough()` entry point

**Step 1: Add fast-path dispatch in passthrough()**

Replace the existing `passthrough()` function (line 1661-1668) with:

```cpp
  void
  passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data) {
    auto *payload = (PNV_INPUT_HEADER) input_data.data();

    // Fast-path: mouse events bypass task_pool entirely
    switch (util::endian::little(payload->magic)) {
      case MOUSE_MOVE_REL_MAGIC_GEN5: {
        if (!config::input.mouse) return;
        auto pkt = (PNV_REL_MOUSE_MOVE_PACKET) payload;
        input->mouse_left_button_timeout = DISABLE_LEFT_BUTTON_DELAY;
        platf::mouse_move_rel_fast(platf_input,
          util::endian::big(pkt->deltaX), util::endian::big(pkt->deltaY));
        return;
      }
      case MOUSE_MOVE_ABS_MAGIC: {
        if (!config::input.mouse) return;
        auto pkt = (PNV_ABS_MOUSE_MOVE_PACKET) payload;
        if (input->mouse_left_button_timeout == DISABLE_LEFT_BUTTON_DELAY) {
          input->mouse_left_button_timeout = ENABLE_LEFT_BUTTON_DELAY;
        }
        if (!pkt->width || !pkt->height) return;
        float x = util::endian::big(pkt->x);
        float y = util::endian::big(pkt->y);
        float w = (float) util::endian::big(pkt->width);
        float h = (float) util::endian::big(pkt->height);
        auto tpcoords = client_to_touchport(input, {x, y}, {w, h});
        if (!tpcoords) return;
        auto &tp = input->touch_port;
        platf::touch_port_t abs_port { tp.offset_x, tp.offset_y, tp.env_width, tp.env_height };
        platf::mouse_move_fast(platf_input, abs_port, tpcoords->first, tpcoords->second);
        return;
      }
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5: {
        if (!config::input.mouse) return;
        auto pkt = (PNV_MOUSE_BUTTON_PACKET) payload;
        auto release = util::endian::little(pkt->header.magic) == MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5;
        auto button = util::endian::big(pkt->button);
        if (button > 0 && button < mouse_press.size()) {
          if (mouse_press[button] != release) return;
          mouse_press[button] = !release;
        }
        platf::mouse_button_fast(platf_input, button, release);
        return;
      }
      default:
        break;
    }

    // All other input: queue through task_pool as before
    {
      std::lock_guard<std::mutex> lg(input->input_queue_lock);
      input->input_queue.push_back(std::move(input_data));
    }
    task_pool.push(passthrough_next_message, input);
  }
```

**Step 2: Build and verify**

Run: `ninja -C build 2>&1 | tail -5`
Expected: Compiles with 0 errors

**Step 3: Commit**

```bash
git add src/input.cpp
git commit -m "feat(input): wire mouse events to fast-path, bypass task_pool queue"
```

---

### Task 6: Test, verify, and clean up

**Step 1: Clean build**

Run: `rm -rf build && cmake -B build -G Ninja -S . && ninja -C build 2>&1 | tail -10`
Expected: Clean build with 0 errors

**Step 2: Start Sunshine and verify mouse thread starts**

Run: `./build/sunshine &>/tmp/sunshine.log &`
Then: `grep "Mouse thread" /tmp/sunshine.log`
Expected: `Mouse thread started (real-time priority)`

**Step 3: Connect from Moonlight and test mouse**

- Connect from Xiaomi Pad 7 Ultra
- Test Bluetooth mouse movement — should feel smoother
- Test touch/trackpad — should still work
- Test mouse clicks (left, right, middle)
- Test scroll wheel
- Check logs: `grep -E "mouse|Mouse|warp" /tmp/sunshine.log | tail -20`

**Step 4: Verify clean disconnect**

- Disconnect from Moonlight
- Check: `grep "Mouse thread stopped" /tmp/sunshine.log`
Expected: Mouse thread stops cleanly

**Step 5: Commit final state**

```bash
git add -A
git commit -m "feat(macos/input): dedicated real-time mouse thread for low-latency input"
```
