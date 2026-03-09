/**
 * @file src/platform/macos/input.cpp
 * @brief Definitions for macOS input handling.
 */
#include "src/input.h"

#import <Carbon/Carbon.h>
#include <chrono>
#include <mach/mach.h>

#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <iostream>
#include <thread>

#include <atomic>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>

/**
 * @brief Delay for a double click, in milliseconds.
 * @todo Make this configurable.
 */
constexpr std::chrono::milliseconds MULTICLICK_DELAY_MS(500);

namespace platf {
  using namespace std::literals;

  constexpr int BUTTON_RING_SIZE = 8;

  struct macos_input_t {
  public:
    CGDirectDisplayID display {};
    CGFloat displayScaling {};
    CGEventSourceRef source {};

    // keyboard related stuff
    CGEventRef kb_event {};
    std::atomic<CGEventFlags> kb_flags {0};

    // mouse related stuff
    CGEventRef mouse_event {};  // mouse event source
    bool mouse_down[3] {};  // mouse button status
    std::chrono::steady_clock::steady_clock::time_point last_mouse_event[3][2];  // timestamp of last mouse events

    // Mouse position cache and sensitivity
    util::point_t cached_mouse_position {};
    bool position_cache_valid {false};
    float mouse_sensitivity {1.0f};

    // Cached display bounds to avoid CGDisplayBounds() system call per event
    CGRect cached_display_bounds {};

    // Track whether we've done the initial cursor warp for absolute positioning.
    // CGWarpMouseCursorPosition has a 250ms throttle built into macOS, so we
    // only use it once on the first abs_mouse call to establish cursor position,
    // then rely on CGEvent for subsequent moves.
    bool abs_mouse_initialized {false};

    // Track whether the display has been validated after prep commands.
    // Once validated, we skip the per-event display bounds check.
    bool display_validated {false};

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
      button_event_t button_ring[8] {};
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
  };

  // A struct to hold a Windows keycode to Mac virtual keycode mapping.
  struct KeyCodeMap {
    int win_keycode;
    int mac_keycode;
  };

  // Customized less operator for using std::lower_bound() on a KeyCodeMap array.
  bool
  operator<(const KeyCodeMap &a, const KeyCodeMap &b) {
    return a.win_keycode < b.win_keycode;
  }

  // clang-format off
const KeyCodeMap kKeyCodesMap[] = {
  { 0x08 /* VKEY_BACK */,                      kVK_Delete              },
  { 0x09 /* VKEY_TAB */,                       kVK_Tab                 },
  { 0x0A /* VKEY_BACKTAB */,                   0x21E4                  },
  { 0x0C /* VKEY_CLEAR */,                     kVK_ANSI_KeypadClear    },
  { 0x0D /* VKEY_RETURN */,                    kVK_Return              },
  { 0x10 /* VKEY_SHIFT */,                     kVK_Shift               },
  { 0x11 /* VKEY_CONTROL */,                   kVK_Control             },
  { 0x12 /* VKEY_MENU */,                      kVK_Option              },
  { 0x13 /* VKEY_PAUSE */,                     -1                      },
  { 0x14 /* VKEY_CAPITAL */,                   kVK_CapsLock            },
  { 0x15 /* VKEY_KANA */,                      kVK_JIS_Kana            },
  { 0x15 /* VKEY_HANGUL */,                    -1                      },
  { 0x17 /* VKEY_JUNJA */,                     -1                      },
  { 0x18 /* VKEY_FINAL */,                     -1                      },
  { 0x19 /* VKEY_HANJA */,                     -1                      },
  { 0x19 /* VKEY_KANJI */,                     -1                      },
  { 0x1B /* VKEY_ESCAPE */,                    kVK_Escape              },
  { 0x1C /* VKEY_CONVERT */,                   -1                      },
  { 0x1D /* VKEY_NONCONVERT */,                -1                      },
  { 0x1E /* VKEY_ACCEPT */,                    -1                      },
  { 0x1F /* VKEY_MODECHANGE */,                -1                      },
  { 0x20 /* VKEY_SPACE */,                     kVK_Space               },
  { 0x21 /* VKEY_PRIOR */,                     kVK_PageUp              },
  { 0x22 /* VKEY_NEXT */,                      kVK_PageDown            },
  { 0x23 /* VKEY_END */,                       kVK_End                 },
  { 0x24 /* VKEY_HOME */,                      kVK_Home                },
  { 0x25 /* VKEY_LEFT */,                      kVK_LeftArrow           },
  { 0x26 /* VKEY_UP */,                        kVK_UpArrow             },
  { 0x27 /* VKEY_RIGHT */,                     kVK_RightArrow          },
  { 0x28 /* VKEY_DOWN */,                      kVK_DownArrow           },
  { 0x29 /* VKEY_SELECT */,                    -1                      },
  { 0x2A /* VKEY_PRINT */,                     -1                      },
  { 0x2B /* VKEY_EXECUTE */,                   -1                      },
  { 0x2C /* VKEY_SNAPSHOT */,                  -1                      },
  { 0x2D /* VKEY_INSERT */,                    kVK_Help                },
  { 0x2E /* VKEY_DELETE */,                    kVK_ForwardDelete       },
  { 0x2F /* VKEY_HELP */,                      kVK_Help                },
  { 0x30 /* VKEY_0 */,                         kVK_ANSI_0              },
  { 0x31 /* VKEY_1 */,                         kVK_ANSI_1              },
  { 0x32 /* VKEY_2 */,                         kVK_ANSI_2              },
  { 0x33 /* VKEY_3 */,                         kVK_ANSI_3              },
  { 0x34 /* VKEY_4 */,                         kVK_ANSI_4              },
  { 0x35 /* VKEY_5 */,                         kVK_ANSI_5              },
  { 0x36 /* VKEY_6 */,                         kVK_ANSI_6              },
  { 0x37 /* VKEY_7 */,                         kVK_ANSI_7              },
  { 0x38 /* VKEY_8 */,                         kVK_ANSI_8              },
  { 0x39 /* VKEY_9 */,                         kVK_ANSI_9              },
  { 0x41 /* VKEY_A */,                         kVK_ANSI_A              },
  { 0x42 /* VKEY_B */,                         kVK_ANSI_B              },
  { 0x43 /* VKEY_C */,                         kVK_ANSI_C              },
  { 0x44 /* VKEY_D */,                         kVK_ANSI_D              },
  { 0x45 /* VKEY_E */,                         kVK_ANSI_E              },
  { 0x46 /* VKEY_F */,                         kVK_ANSI_F              },
  { 0x47 /* VKEY_G */,                         kVK_ANSI_G              },
  { 0x48 /* VKEY_H */,                         kVK_ANSI_H              },
  { 0x49 /* VKEY_I */,                         kVK_ANSI_I              },
  { 0x4A /* VKEY_J */,                         kVK_ANSI_J              },
  { 0x4B /* VKEY_K */,                         kVK_ANSI_K              },
  { 0x4C /* VKEY_L */,                         kVK_ANSI_L              },
  { 0x4D /* VKEY_M */,                         kVK_ANSI_M              },
  { 0x4E /* VKEY_N */,                         kVK_ANSI_N              },
  { 0x4F /* VKEY_O */,                         kVK_ANSI_O              },
  { 0x50 /* VKEY_P */,                         kVK_ANSI_P              },
  { 0x51 /* VKEY_Q */,                         kVK_ANSI_Q              },
  { 0x52 /* VKEY_R */,                         kVK_ANSI_R              },
  { 0x53 /* VKEY_S */,                         kVK_ANSI_S              },
  { 0x54 /* VKEY_T */,                         kVK_ANSI_T              },
  { 0x55 /* VKEY_U */,                         kVK_ANSI_U              },
  { 0x56 /* VKEY_V */,                         kVK_ANSI_V              },
  { 0x57 /* VKEY_W */,                         kVK_ANSI_W              },
  { 0x58 /* VKEY_X */,                         kVK_ANSI_X              },
  { 0x59 /* VKEY_Y */,                         kVK_ANSI_Y              },
  { 0x5A /* VKEY_Z */,                         kVK_ANSI_Z              },
  { 0x5B /* VKEY_LWIN */,                      kVK_Command             },
  { 0x5C /* VKEY_RWIN */,                      kVK_RightCommand        },
  { 0x5D /* VKEY_APPS */,                      kVK_RightCommand        },
  { 0x5F /* VKEY_SLEEP */,                     -1                      },
  { 0x60 /* VKEY_NUMPAD0 */,                   kVK_ANSI_Keypad0        },
  { 0x61 /* VKEY_NUMPAD1 */,                   kVK_ANSI_Keypad1        },
  { 0x62 /* VKEY_NUMPAD2 */,                   kVK_ANSI_Keypad2        },
  { 0x63 /* VKEY_NUMPAD3 */,                   kVK_ANSI_Keypad3        },
  { 0x64 /* VKEY_NUMPAD4 */,                   kVK_ANSI_Keypad4        },
  { 0x65 /* VKEY_NUMPAD5 */,                   kVK_ANSI_Keypad5        },
  { 0x66 /* VKEY_NUMPAD6 */,                   kVK_ANSI_Keypad6        },
  { 0x67 /* VKEY_NUMPAD7 */,                   kVK_ANSI_Keypad7        },
  { 0x68 /* VKEY_NUMPAD8 */,                   kVK_ANSI_Keypad8        },
  { 0x69 /* VKEY_NUMPAD9 */,                   kVK_ANSI_Keypad9        },
  { 0x6A /* VKEY_MULTIPLY */,                  kVK_ANSI_KeypadMultiply },
  { 0x6B /* VKEY_ADD */,                       kVK_ANSI_KeypadPlus     },
  { 0x6C /* VKEY_SEPARATOR */,                 -1                      },
  { 0x6D /* VKEY_SUBTRACT */,                  kVK_ANSI_KeypadMinus    },
  { 0x6E /* VKEY_DECIMAL */,                   kVK_ANSI_KeypadDecimal  },
  { 0x6F /* VKEY_DIVIDE */,                    kVK_ANSI_KeypadDivide   },
  { 0x70 /* VKEY_F1 */,                        kVK_F1                  },
  { 0x71 /* VKEY_F2 */,                        kVK_F2                  },
  { 0x72 /* VKEY_F3 */,                        kVK_F3                  },
  { 0x73 /* VKEY_F4 */,                        kVK_F4                  },
  { 0x74 /* VKEY_F5 */,                        kVK_F5                  },
  { 0x75 /* VKEY_F6 */,                        kVK_F6                  },
  { 0x76 /* VKEY_F7 */,                        kVK_F7                  },
  { 0x77 /* VKEY_F8 */,                        kVK_F8                  },
  { 0x78 /* VKEY_F9 */,                        kVK_F9                  },
  { 0x79 /* VKEY_F10 */,                       kVK_F10                 },
  { 0x7A /* VKEY_F11 */,                       kVK_F11                 },
  { 0x7B /* VKEY_F12 */,                       kVK_F12                 },
  { 0x7C /* VKEY_F13 */,                       kVK_F13                 },
  { 0x7D /* VKEY_F14 */,                       kVK_F14                 },
  { 0x7E /* VKEY_F15 */,                       kVK_F15                 },
  { 0x7F /* VKEY_F16 */,                       kVK_F16                 },
  { 0x80 /* VKEY_F17 */,                       kVK_F17                 },
  { 0x81 /* VKEY_F18 */,                       kVK_F18                 },
  { 0x82 /* VKEY_F19 */,                       kVK_F19                 },
  { 0x83 /* VKEY_F20 */,                       kVK_F20                 },
  { 0x84 /* VKEY_F21 */,                       -1                      },
  { 0x85 /* VKEY_F22 */,                       -1                      },
  { 0x86 /* VKEY_F23 */,                       -1                      },
  { 0x87 /* VKEY_F24 */,                       -1                      },
  { 0x90 /* VKEY_NUMLOCK */,                   -1                      },
  { 0x91 /* VKEY_SCROLL */,                    -1                      },
  { 0xA0 /* VKEY_LSHIFT */,                    kVK_Shift               },
  { 0xA1 /* VKEY_RSHIFT */,                    kVK_RightShift          },
  { 0xA2 /* VKEY_LCONTROL */,                  kVK_Control             },
  { 0xA3 /* VKEY_RCONTROL */,                  kVK_RightControl        },
  { 0xA4 /* VKEY_LMENU */,                     kVK_Option              },
  { 0xA5 /* VKEY_RMENU */,                     kVK_RightOption         },
  { 0xA6 /* VKEY_BROWSER_BACK */,              -1                      },
  { 0xA7 /* VKEY_BROWSER_FORWARD */,           -1                      },
  { 0xA8 /* VKEY_BROWSER_REFRESH */,           -1                      },
  { 0xA9 /* VKEY_BROWSER_STOP */,              -1                      },
  { 0xAA /* VKEY_BROWSER_SEARCH */,            -1                      },
  { 0xAB /* VKEY_BROWSER_FAVORITES */,         -1                      },
  { 0xAC /* VKEY_BROWSER_HOME */,              -1                      },
  { 0xAD /* VKEY_VOLUME_MUTE */,               -1                      },
  { 0xAE /* VKEY_VOLUME_DOWN */,               -1                      },
  { 0xAF /* VKEY_VOLUME_UP */,                 -1                      },
  { 0xB0 /* VKEY_MEDIA_NEXT_TRACK */,          -1                      },
  { 0xB1 /* VKEY_MEDIA_PREV_TRACK */,          -1                      },
  { 0xB2 /* VKEY_MEDIA_STOP */,                -1                      },
  { 0xB3 /* VKEY_MEDIA_PLAY_PAUSE */,          -1                      },
  { 0xB4 /* VKEY_MEDIA_LAUNCH_MAIL */,         -1                      },
  { 0xB5 /* VKEY_MEDIA_LAUNCH_MEDIA_SELECT */, -1                      },
  { 0xB6 /* VKEY_MEDIA_LAUNCH_APP1 */,         -1                      },
  { 0xB7 /* VKEY_MEDIA_LAUNCH_APP2 */,         -1                      },
  { 0xBA /* VKEY_OEM_1 */,                     kVK_ANSI_Semicolon      },
  { 0xBB /* VKEY_OEM_PLUS */,                  kVK_ANSI_Equal          },
  { 0xBC /* VKEY_OEM_COMMA */,                 kVK_ANSI_Comma          },
  { 0xBD /* VKEY_OEM_MINUS */,                 kVK_ANSI_Minus          },
  { 0xBE /* VKEY_OEM_PERIOD */,                kVK_ANSI_Period         },
  { 0xBF /* VKEY_OEM_2 */,                     kVK_ANSI_Slash          },
  { 0xC0 /* VKEY_OEM_3 */,                     kVK_ANSI_Grave          },
  { 0xDB /* VKEY_OEM_4 */,                     kVK_ANSI_LeftBracket    },
  { 0xDC /* VKEY_OEM_5 */,                     kVK_ANSI_Backslash      },
  { 0xDD /* VKEY_OEM_6 */,                     kVK_ANSI_RightBracket   },
  { 0xDE /* VKEY_OEM_7 */,                     kVK_ANSI_Quote          },
  { 0xDF /* VKEY_OEM_8 */,                     -1                      },
  { 0xE2 /* VKEY_OEM_102 */,                   -1                      },
  { 0xE5 /* VKEY_PROCESSKEY */,                -1                      },
  { 0xE7 /* VKEY_PACKET */,                    -1                      },
  { 0xF6 /* VKEY_ATTN */,                      -1                      },
  { 0xF7 /* VKEY_CRSEL */,                     -1                      },
  { 0xF8 /* VKEY_EXSEL */,                     -1                      },
  { 0xF9 /* VKEY_EREOF */,                     -1                      },
  { 0xFA /* VKEY_PLAY */,                      -1                      },
  { 0xFB /* VKEY_ZOOM */,                      -1                      },
  { 0xFC /* VKEY_NONAME */,                    -1                      },
  { 0xFD /* VKEY_PA1 */,                       -1                      },
  { 0xFE /* VKEY_OEM_CLEAR */,                 kVK_ANSI_KeypadClear    }
};
  // clang-format on

  int
  keysym(int keycode) {
    KeyCodeMap key_map {};

    key_map.win_keycode = keycode;
    const KeyCodeMap *temp_map = std::lower_bound(
      kKeyCodesMap, kKeyCodesMap + sizeof(kKeyCodesMap) / sizeof(kKeyCodesMap[0]), key_map);

    if (temp_map >= kKeyCodesMap + sizeof(kKeyCodesMap) / sizeof(kKeyCodesMap[0]) ||
        temp_map->win_keycode != keycode || temp_map->mac_keycode == -1) {
      return -1;
    }

    return temp_map->mac_keycode;
  }

  void
  keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t flags) {
    auto key = keysym(modcode);

    BOOST_LOG(debug) << "got keycode: 0x"sv << std::hex << modcode << ", translated to: 0x" << std::hex << key << ", release:" << release;

    if (key < 0) {
      return;
    }

    auto macos_input = ((macos_input_t *) input.get());
    auto event = macos_input->kb_event;

    if (key == kVK_Shift || key == kVK_RightShift ||
        key == kVK_Command || key == kVK_RightCommand ||
        key == kVK_Option || key == kVK_RightOption ||
        key == kVK_Control || key == kVK_RightControl) {
      CGEventFlags mask;

      switch (key) {
        case kVK_Shift:
        case kVK_RightShift:
          mask = kCGEventFlagMaskShift;
          break;
        case kVK_Command:
        case kVK_RightCommand:
          mask = kCGEventFlagMaskCommand;
          break;
        case kVK_Option:
        case kVK_RightOption:
          mask = kCGEventFlagMaskAlternate;
          break;
        case kVK_Control:
        case kVK_RightControl:
          mask = kCGEventFlagMaskControl;
          break;
      }

      CGEventFlags old_flags = macos_input->kb_flags.load(std::memory_order_relaxed);
      CGEventFlags new_flags = release ? old_flags & ~mask : old_flags | mask;
      macos_input->kb_flags.store(new_flags, std::memory_order_relaxed);
      CGEventSetType(event, kCGEventFlagsChanged);
      CGEventSetFlags(event, new_flags);
    }
    else {
      CGEventSetIntegerValueField(event, kCGKeyboardEventKeycode, key);
      CGEventSetType(event, release ? kCGEventKeyUp : kCGEventKeyDown);

      // Arrow keys, function keys, and navigation keys on Apple keyboards
      // carry kCGEventFlagMaskSecondaryFn. Without it, macOS system shortcuts
      // (e.g. Ctrl+Arrow for Spaces switching) won't trigger.
      CGEventFlags flags = macos_input->kb_flags.load(std::memory_order_relaxed);
      switch (key) {
        case kVK_LeftArrow: case kVK_RightArrow:
        case kVK_UpArrow: case kVK_DownArrow:
        case kVK_Home: case kVK_End:
        case kVK_PageUp: case kVK_PageDown:
        case kVK_ForwardDelete:
        case kVK_F1: case kVK_F2: case kVK_F3: case kVK_F4:
        case kVK_F5: case kVK_F6: case kVK_F7: case kVK_F8:
        case kVK_F9: case kVK_F10: case kVK_F11: case kVK_F12:
        case kVK_F13: case kVK_F14: case kVK_F15: case kVK_F16:
        case kVK_F17: case kVK_F18: case kVK_F19: case kVK_F20:
          flags |= kCGEventFlagMaskSecondaryFn;
          break;
      }
      CGEventSetFlags(event, flags);
    }

    CGEventPost(kCGHIDEventTap, event);
  }

  void
  unicode(input_t &input, char *utf8, int size) {
    BOOST_LOG(info) << "unicode: Unicode input not yet implemented for MacOS."sv;
  }

  int
  alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue) {
    BOOST_LOG(info) << "alloc_gamepad: Gamepad not yet implemented for MacOS."sv;
    return -1;
  }

  void
  free_gamepad(input_t &input, int nr) {
    BOOST_LOG(info) << "free_gamepad: Gamepad not yet implemented for MacOS."sv;
  }

  void
  gamepad_update(input_t &input, int nr, const gamepad_state_t &gamepad_state) {
    BOOST_LOG(info) << "gamepad: Gamepad not yet implemented for MacOS."sv;
  }

  // returns current mouse location:
  util::point_t
  get_mouse_loc(input_t &input) {
    const auto macos_input = static_cast<macos_input_t *>(input.get());

    // 如果缓存有效，直接返回
    if (macos_input->position_cache_valid) {
      return macos_input->cached_mouse_position;
    }

    // 缓存失效，查询系统
    const auto snapshot_event = CGEventCreate(NULL);
    const auto current = CGEventGetLocation(snapshot_event);
    CFRelease(snapshot_event);

    // 更新缓存
    macos_input->cached_mouse_position = util::point_t { current.x, current.y };
    macos_input->position_cache_valid = true;

    return macos_input->cached_mouse_position;
  }

  void
  post_mouse(
    input_t &input,
    const CGMouseButton button,
    const CGEventType type,
    const util::point_t raw_location,
    const util::point_t previous_location,
    const int click_count) {
    const auto macos_input = static_cast<macos_input_t *>(input.get());

    // Use cached display bounds (updated in refresh_display_if_needed / input init)
    const auto &display_bounds = macos_input->cached_display_bounds;

    // Clamp mouse within display bounds
    const auto location = CGPoint {
      std::clamp(raw_location.x, display_bounds.origin.x, display_bounds.origin.x + display_bounds.size.width - 1),
      std::clamp(raw_location.y, display_bounds.origin.y, display_bounds.origin.y + display_bounds.size.height - 1)
    };

    // Compute deltas for 3D apps / games
    const double deltaX = raw_location.x - previous_location.x;
    const double deltaY = raw_location.y - previous_location.y;

    // Button events: create fresh CGEvent each time (original behavior).
    // Reusing a mouse-move event for button events can leak state and cause
    // macOS to not register clicks properly.
    bool is_button = (type == kCGEventLeftMouseDown || type == kCGEventLeftMouseUp ||
                      type == kCGEventRightMouseDown || type == kCGEventRightMouseUp ||
                      type == kCGEventOtherMouseDown || type == kCGEventOtherMouseUp);

    CGEventRef event;
    if (is_button) {
      event = CGEventCreateMouseEvent(macos_input->source, type, location, button);
      CGEventSetIntegerValueField(event, kCGMouseEventClickState, click_count);
      CGEventSetDoubleValueField(event, kCGMouseEventDeltaX, deltaX);
      CGEventSetDoubleValueField(event, kCGMouseEventDeltaY, deltaY);
      CGEventPost(kCGHIDEventTap, event);
      CFRelease(event);
    }
    else {
      event = macos_input->mouse_event;
      CGEventSetType(event, type);
      CGEventSetLocation(event, location);
      CGEventSetIntegerValueField(event, kCGMouseEventButtonNumber, button);
      CGEventSetIntegerValueField(event, kCGMouseEventClickState, click_count);
      CGEventSetDoubleValueField(event, kCGMouseEventDeltaX, deltaX);
      CGEventSetDoubleValueField(event, kCGMouseEventDeltaY, deltaY);
      CGEventPost(kCGHIDEventTap, event);
    }

    // Update position cache
    macos_input->cached_mouse_position = util::point_t { location.x, location.y };
    macos_input->position_cache_valid = true;
  }

  inline CGEventType
  event_type_mouse(input_t &input) {
    const auto macos_input = static_cast<macos_input_t *>(input.get());

    if (macos_input->mouse_down[0]) {
      return kCGEventLeftMouseDragged;
    }
    if (macos_input->mouse_down[1]) {
      return kCGEventOtherMouseDragged;
    }
    if (macos_input->mouse_down[2]) {
      return kCGEventRightMouseDragged;
    }
    return kCGEventMouseMoved;
  }

  void
  move_mouse(
    input_t &input,
    const int deltaX,
    const int deltaY) {
    const auto macos_input = static_cast<macos_input_t *>(input.get());

    const auto current = macos_input->position_cache_valid ? macos_input->cached_mouse_position : get_mouse_loc(input);

    const float sensitivity = macos_input->mouse_sensitivity;
    const auto location = util::point_t {
      current.x + deltaX * sensitivity,
      current.y + deltaY * sensitivity
    };

    post_mouse(input, kCGMouseButtonLeft, event_type_mouse(input), location, current, 0);
  }

  /**
   * @brief Refresh the input display if the current one has become invalid.
   * @details After prep commands (e.g., sunshine_connect.sh) switch displays,
   * the original display may become a 1x1 ghost. Detect this and switch to
   * the current main display.
   */
  void
  refresh_display_if_needed(macos_input_t *macos_input) {
    // Once display is validated, no need to check again
    if (macos_input->display_validated) {
      return;
    }

    CGRect bounds = CGDisplayBounds(macos_input->display);
    if (bounds.size.width > 1 && bounds.size.height > 1) {
      macos_input->cached_display_bounds = bounds;
      macos_input->display_validated = true;
      return;  // Current display is valid
    }

    // Display became invalid (disconnected/ghost). Switch to main display.
    CGDirectDisplayID new_display = CGMainDisplayID();
    if (new_display == macos_input->display) {
      // Main display is also invalid, scan for any valid display
      constexpr uint32_t max_display = 32;
      uint32_t display_count;
      CGDirectDisplayID displays[max_display];
      if (CGGetActiveDisplayList(max_display, displays, &display_count) == kCGErrorSuccess) {
        for (uint32_t i = 0; i < display_count; i++) {
          CGRect db = CGDisplayBounds(displays[i]);
          if (db.size.width > 1 && db.size.height > 1) {
            new_display = displays[i];
            break;
          }
        }
      }
    }

    macos_input->display = new_display;
    const CGDisplayModeRef mode = CGDisplayCopyDisplayMode(new_display);
    if (mode) {
      macos_input->displayScaling = ((CGFloat) CGDisplayPixelsWide(new_display)) / ((CGFloat) CGDisplayModeGetPixelWidth(mode));
      CFRelease(mode);
    }

    CGRect new_bounds = CGDisplayBounds(new_display);
    macos_input->cached_display_bounds = new_bounds;
    BOOST_LOG(info) << "Input display refreshed: display=" << new_display
                    << " scaling=" << macos_input->displayScaling
                    << " bounds=(" << new_bounds.origin.x << "," << new_bounds.origin.y
                    << " " << new_bounds.size.width << "x" << new_bounds.size.height << ")";

    // Reset warp state so the cursor gets warped to the new display
    macos_input->abs_mouse_initialized = false;
    macos_input->display_validated = true;
  }

  void
  abs_mouse(
    input_t &input,
    const touch_port_t &touch_port,
    const float x,
    const float y) {
    const auto macos_input = static_cast<macos_input_t *>(input.get());

    // Check if display became invalid after prep commands switched displays
    refresh_display_if_needed(macos_input);

    const auto scaling = macos_input->displayScaling;

    // Use cached bounds instead of CGDisplayBounds() system call
    const auto &display_bounds = macos_input->cached_display_bounds;
    auto location = util::point_t {
      x * scaling + display_bounds.origin.x,
      y * scaling + display_bounds.origin.y
    };

    if (!macos_input->abs_mouse_initialized) {
      CGAssociateMouseAndMouseCursorPosition(false);
      CGWarpMouseCursorPosition(CGPoint { location.x, location.y });
      CGAssociateMouseAndMouseCursorPosition(true);

      macos_input->abs_mouse_initialized = true;
      BOOST_LOG(info) << "abs_mouse: initial cursor warp to ("sv << location.x << ", "sv << location.y << ")"sv;
    }

    // Use cached position directly — avoids creating a CGEvent snapshot per move
    post_mouse(input, kCGMouseButtonLeft, event_type_mouse(input), location, macos_input->cached_mouse_position, 0);
  }

  void
  button_mouse(input_t &input, const int button, const bool release) {
    CGMouseButton mac_button;
    CGEventType event;

    const auto macos_input = static_cast<macos_input_t *>(input.get());

    switch (button) {
      case 1:
        mac_button = kCGMouseButtonLeft;
        event = release ? kCGEventLeftMouseUp : kCGEventLeftMouseDown;
        break;
      case 2:
        mac_button = kCGMouseButtonCenter;
        event = release ? kCGEventOtherMouseUp : kCGEventOtherMouseDown;
        break;
      case 3:
        mac_button = kCGMouseButtonRight;
        event = release ? kCGEventRightMouseUp : kCGEventRightMouseDown;
        break;
      default:
        BOOST_LOG(warning) << "Unsupported mouse button for MacOS: "sv << button;
        return;
    }

    macos_input->mouse_down[mac_button] = !release;

    // if the last mouse down was less than MULTICLICK_DELAY_MS, we send a double click event
    const auto now = std::chrono::steady_clock::now();
    const auto mouse_position = get_mouse_loc(input);

    if (now < macos_input->last_mouse_event[mac_button][release] + MULTICLICK_DELAY_MS) {
      post_mouse(input, mac_button, event, mouse_position, mouse_position, 2);
    }
    else {
      post_mouse(input, mac_button, event, mouse_position, mouse_position, 1);
    }

    macos_input->last_mouse_event[mac_button][release] = now;
  }

  // --- Fast-path mouse functions (called from control stream thread, lock-free) ---

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

      // Initialize atomic prev to warp position so first event has zero delta
      mi->mouse_state.abs_x.store(ax, std::memory_order_relaxed);
      mi->mouse_state.abs_y.store(ay, std::memory_order_relaxed);

      // Post an immediate CGEvent on this thread to unhide cursor after warp
      // (macOS hides cursor after CGWarpMouseCursorPosition until a "real" event)
      CGEventRef ev = mi->mouse_event;
      CGEventSetType(ev, kCGEventMouseMoved);
      CGEventSetLocation(ev, CGPoint { ax, ay });
      CGEventSetIntegerValueField(ev, kCGMouseEventButtonNumber, kCGMouseButtonLeft);
      CGEventSetIntegerValueField(ev, kCGMouseEventClickState, 0);
      CGEventSetDoubleValueField(ev, kCGMouseEventDeltaX, 0);
      CGEventSetDoubleValueField(ev, kCGMouseEventDeltaY, 0);
      CGEventPost(kCGHIDEventTap, ev);

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
    if (wi - ri >= BUTTON_RING_SIZE) return;

    auto &slot = mi->mouse_state.button_ring[wi % BUTTON_RING_SIZE];
    slot.button = button;
    slot.release = release;
    mi->mouse_state.button_write_idx.store(wi + 1, std::memory_order_release);
    dispatch_semaphore_signal(mi->mouse_semaphore);
  }

  void
  scroll(input_t &input, const int high_res_distance) {
    // Convert high_res_distance to scroll pixels
    // Moonlight sends high_res_distance in units of 120 per "click"
    // Modern macOS expects pixel-based scrolling for smooth, precise control
    // Scale factor: 120 units ≈ 10 pixels (typical scroll distance)
    const double pixelsPerUnit = 10.0 / 120.0;
    int32_t scrollPixels = static_cast<int32_t>(high_res_distance * pixelsPerUnit);

    // If the amount is too small, use at least ±1 for any non-zero input
    if (scrollPixels == 0 && high_res_distance != 0) {
      scrollPixels = high_res_distance > 0 ? 1 : -1;
    }

    CGEventRef scrollEvent = CGEventCreateScrollWheelEvent(
      nullptr,
      kCGScrollEventUnitPixel,  // Use pixel units for modern smooth scrolling
      1,  // wheelCount: 1 for vertical scroll only
      scrollPixels);  // vertical scroll amount in pixels

    CGEventPost(kCGHIDEventTap, scrollEvent);
    CFRelease(scrollEvent);
  }

  void
  hscroll(input_t &input, int high_res_distance) {
    // Unimplemented
  }

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

    // Create a fresh CGEvent for button events to avoid state leakage from reused event.
    // Move events can still reuse mt_event for performance.
    bool is_button = (type == kCGEventLeftMouseDown || type == kCGEventLeftMouseUp ||
                      type == kCGEventRightMouseDown || type == kCGEventRightMouseUp ||
                      type == kCGEventOtherMouseDown || type == kCGEventOtherMouseUp);

    CGEventRef ev;
    if (is_button) {
      ev = CGEventCreateMouseEvent(mi->mt_source, type, loc, button);
      CGEventSetIntegerValueField(ev, kCGMouseEventClickState, click_count);
      CGEventSetFlags(ev, mi->kb_flags.load(std::memory_order_relaxed));
      BOOST_LOG(info) << "mt_post_event: button type=" << type << " btn=" << button
                       << " click=" << click_count << " at (" << loc.x << "," << loc.y << ")";
    }
    else {
      ev = mi->mt_event;
      CGEventSetType(ev, type);
      CGEventSetLocation(ev, loc);
      CGEventSetIntegerValueField(ev, kCGMouseEventButtonNumber, button);
      CGEventSetIntegerValueField(ev, kCGMouseEventClickState, click_count);
      CGEventSetDoubleValueField(ev, kCGMouseEventDeltaX, x - prev_x);
      CGEventSetDoubleValueField(ev, kCGMouseEventDeltaY, y - prev_y);
      CGEventSetFlags(ev, mi->kb_flags.load(std::memory_order_relaxed));
    }
    CGEventPost(kCGHIDEventTap, ev);
    if (is_button) {
      CFRelease(ev);
    }

    mi->mt_pos_x = loc.x;
    mi->mt_pos_y = loc.y;
  }

  void
  mouse_thread_func(macos_input_t *mi) {
    thread_time_constraint_policy_data_t policy;
    policy.period = 1000000;
    policy.computation = 250000;
    policy.constraint = 500000;
    policy.preemptible = true;
    thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t) &policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);

    BOOST_LOG(info) << "Mouse thread started (real-time priority)";

    bool mouse_down[3] {};
    uint32_t last_move_seq = 0;

    // Double-click detection: track last button event time per button per direction
    std::chrono::steady_clock::time_point last_btn_time[3][2] {};  // [button_idx][0=down,1=up]

    while (mi->mouse_thread_running.load(std::memory_order_relaxed)) {
      dispatch_semaphore_wait(mi->mouse_semaphore,
                              dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_MSEC));

      uint32_t br = mi->mouse_state.button_read_idx.load(std::memory_order_relaxed);
      uint32_t bw = mi->mouse_state.button_write_idx.load(std::memory_order_acquire);
      while (br != bw) {
        auto &evt = mi->mouse_state.button_ring[br % BUTTON_RING_SIZE];
        int btn = evt.button;
        bool release = evt.release;

        CGMouseButton mac_button;
        CGEventType etype;
        int btn_idx = 0;
        switch (btn) {
          case 1:
            mac_button = kCGMouseButtonLeft; etype = release ? kCGEventLeftMouseUp : kCGEventLeftMouseDown; btn_idx = 0; break;
          case 2:
            mac_button = kCGMouseButtonCenter; etype = release ? kCGEventOtherMouseUp : kCGEventOtherMouseDown; btn_idx = 1; break;
          case 3:
            mac_button = kCGMouseButtonRight; etype = release ? kCGEventRightMouseUp : kCGEventRightMouseDown; btn_idx = 2; break;
          default:
            br++; continue;
        }
        mouse_down[btn_idx] = !release;

        // Double-click detection: if same button event within MULTICLICK_DELAY_MS, send click_count=2
        auto now = std::chrono::steady_clock::now();
        int click_count = 1;
        int dir = release ? 1 : 0;
        if (now < last_btn_time[btn_idx][dir] + MULTICLICK_DELAY_MS) {
          click_count = 2;
        }
        last_btn_time[btn_idx][dir] = now;

        mt_post_event(mi, etype, mac_button, mi->mt_pos_x, mi->mt_pos_y,
                      mi->mt_pos_x, mi->mt_pos_y, click_count);
        br++;
      }
      mi->mouse_state.button_read_idx.store(br, std::memory_order_release);

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

  /**
   * @brief Allocates a context to store per-client input data.
   * @param input The global input context.
   * @return A unique pointer to a per-client input data context.
   */
  std::unique_ptr<client_input_t>
  allocate_client_input_context(input_t &input) {
    // Unused
    return nullptr;
  }

  /**
   * @brief Sends a touch event to the OS.
   * @param input The client-specific input context.
   * @param touch_port The current viewport for translating to screen coordinates.
   * @param touch The touch event.
   */
  void
  touch_update(client_input_t *input, const touch_port_t &touch_port, const touch_input_t &touch) {
    // Unimplemented feature - platform_caps::pen_touch
  }

  /**
   * @brief Sends a pen event to the OS.
   * @param input The client-specific input context.
   * @param touch_port The current viewport for translating to screen coordinates.
   * @param pen The pen event.
   */
  void
  pen_update(client_input_t *input, const touch_port_t &touch_port, const pen_input_t &pen) {
    // Unimplemented feature - platform_caps::pen_touch
  }

  /**
   * @brief Sends a gamepad touch event to the OS.
   * @param input The global input context.
   * @param touch The touch event.
   */
  void
  gamepad_touch(input_t &input, const gamepad_touch_t &touch) {
    // Unimplemented feature - platform_caps::controller_touch
  }

  /**
   * @brief Sends a gamepad motion event to the OS.
   * @param input The global input context.
   * @param motion The motion event.
   */
  void
  gamepad_motion(input_t &input, const gamepad_motion_t &motion) {
    // Unimplemented
  }

  /**
   * @brief Sends a gamepad battery event to the OS.
   * @param input The global input context.
   * @param battery The battery event.
   */
  void
  gamepad_battery(input_t &input, const gamepad_battery_t &battery) {
    // Unimplemented
  }

  input_t
  input() {
    input_t result { new macos_input_t() };

    const auto macos_input = static_cast<macos_input_t *>(result.get());

    // Default to main display
    macos_input->display = CGMainDisplayID();

    auto output_name = config::video.output_name;
    // If output_name is set, try to find the display with that display id
    if (!output_name.empty()) {
      constexpr uint32_t max_display = 32;
      uint32_t display_count;
      CGDirectDisplayID displays[max_display];
      if (CGGetActiveDisplayList(max_display, displays, &display_count) != kCGErrorSuccess) {
        BOOST_LOG(error) << "Unable to get active display list , error: "sv << std::endl;
      }
      else {
        for (int i = 0; i < display_count; i++) {
          CGDirectDisplayID display_id = displays[i];
          if (display_id == std::atoi(output_name.c_str())) {
            macos_input->display = display_id;
          }
        }
      }
    }

    // Input coordinates are based on the virtual resolution not the physical, so we need the scaling factor
    const CGDisplayModeRef mode = CGDisplayCopyDisplayMode(macos_input->display);
    macos_input->displayScaling = ((CGFloat) CGDisplayPixelsWide(macos_input->display)) / ((CGFloat) CGDisplayModeGetPixelWidth(mode));
    CFRelease(mode);

    CGRect bounds = CGDisplayBounds(macos_input->display);
    macos_input->cached_display_bounds = bounds;
    BOOST_LOG(info) << "Input initialized: display=" << macos_input->display
                    << " mainDisplay=" << CGMainDisplayID()
                    << " scaling=" << macos_input->displayScaling
                    << " bounds=(" << bounds.origin.x << "," << bounds.origin.y
                    << " " << bounds.size.width << "x" << bounds.size.height << ")"
                    << " postEventAccess=" << CGPreflightPostEventAccess();

    macos_input->source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);

    // Set local events suppression interval to 0 to avoid initial delay
    // By default, macOS has a 250ms suppression interval which causes
    // programmatically generated events to be delayed or ignored initially
    CGEventSourceSetLocalEventsSuppressionInterval(macos_input->source, 0.0);

    macos_input->kb_event = CGEventCreate(macos_input->source);
    macos_input->kb_flags.store(0, std::memory_order_relaxed);

    // Create a reusable mouse event — post_mouse modifies and reuses this
    // instead of creating/releasing a new CGEvent per mouse move
    macos_input->mouse_event = CGEventCreateMouseEvent(macos_input->source, kCGEventMouseMoved, CGPointZero, kCGMouseButtonLeft);
    macos_input->mouse_down[0] = false;
    macos_input->mouse_down[1] = false;
    macos_input->mouse_down[2] = false;

    macos_input->mouse_sensitivity = config::input_mouse_sensitivity;
    macos_input->position_cache_valid = false;

    // Create dedicated mouse thread with its own CGEvent resources
    macos_input->mouse_semaphore = dispatch_semaphore_create(0);
    macos_input->mt_source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    CGEventSourceSetLocalEventsSuppressionInterval(macos_input->mt_source, 0.0);

    // Initialize mouse thread position to actual cursor location so that
    // relative-mode clicks and moves start from the correct position
    // (not (0,0) which would cause cursor jump and misplaced clicks)
    CGEventRef pos_event = CGEventCreate(NULL);
    CGPoint cur_pos = CGEventGetLocation(pos_event);
    CFRelease(pos_event);
    macos_input->mt_pos_x = cur_pos.x;
    macos_input->mt_pos_y = cur_pos.y;

    macos_input->mt_event = CGEventCreateMouseEvent(macos_input->mt_source, kCGEventMouseMoved, cur_pos, kCGMouseButtonLeft);
    macos_input->mouse_thread_running.store(true, std::memory_order_relaxed);
    macos_input->mouse_thread = std::thread(mouse_thread_func, macos_input);

    BOOST_LOG(debug) << "Mouse sensitivity set to: " << macos_input->mouse_sensitivity;

    BOOST_LOG(debug) << "Display "sv << macos_input->display << ", pixel dimension: " << CGDisplayPixelsWide(macos_input->display) << "x"sv << CGDisplayPixelsHigh(macos_input->display);

    return result;
  }

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

  std::vector<supported_gamepad_t> &
  supported_gamepads(input_t *input) {
    static std::vector gamepads {
      supported_gamepad_t { "", false, "gamepads.macos_not_implemented" }
    };

    return gamepads;
  }

  /**
   * @brief Returns the supported platform capabilities to advertise to the client.
   * @return Capability flags.
   */
  platform_caps::caps_t
  get_capabilities() {
    return 0;
  }
}  // namespace platf
