/**
 * @file src/platform/windows/input_hid.cpp
 * @brief Zako Input HID Bus client implementation.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <initguid.h>
#include <setupapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "input_hid.h"
#include "src/logging.h"

namespace {
  using namespace std::literals;

  // {2C4ACAA5-449E-48D7-8B40-57C79EF9B4A4}
  DEFINE_GUID(
    GUID_DEVINTERFACE_ZAKO_INPUT_HID,
    0x2c4acaa5,
    0x449e,
    0x48d7,
    0x8b, 0x40, 0x57, 0xc7, 0x9e, 0xf9, 0xb4, 0xa4);

  constexpr DWORD ZAKO_INPUT_HID_DEVICE_TYPE = 0x8337;
  constexpr uint8_t ZAKO_HID_MOUSE_REPORT_ID = 0x01;
  constexpr uint8_t ZAKO_HID_KEYBOARD_REPORT_ID = 0x02;

  constexpr DWORD IOCTL_ZAKO_INPUT_HID_SUBMIT_MOUSE =
    CTL_CODE(ZAKO_INPUT_HID_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_WRITE_DATA);
  constexpr DWORD IOCTL_ZAKO_INPUT_HID_SUBMIT_KEYBOARD =
    CTL_CODE(ZAKO_INPUT_HID_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_WRITE_DATA);
  constexpr DWORD IOCTL_ZAKO_INPUT_HID_RELEASE_ALL =
    CTL_CODE(ZAKO_INPUT_HID_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_WRITE_DATA);

#pragma pack(push, 1)
  struct mouse_report_t {
    uint8_t report_id;
    uint8_t buttons;
    int16_t delta_x;
    int16_t delta_y;
    int8_t wheel_vertical;
    int8_t wheel_horizontal;
  };

  struct keyboard_report_t {
    uint8_t report_id;
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[6];
  };

  struct mouse_submit_t {
    uint32_t sequence;
    mouse_report_t report;
  };

  struct keyboard_submit_t {
    uint32_t sequence;
    keyboard_report_t report;
  };

  struct release_all_t {
    uint32_t sequence;
    uint32_t reason;
  };
#pragma pack(pop)

  static_assert(sizeof(mouse_report_t) == 8);
  static_assert(sizeof(keyboard_report_t) == 9);

  std::string
  wide_to_utf8(const wchar_t *wstr) {
    if (!wstr) {
      return "";
    }

    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
      return "";
    }

    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), len, nullptr, nullptr);
    return result;
  }

  class setup_info_set_t {
  public:
    explicit setup_info_set_t(HDEVINFO set) noexcept:
        set { set } {}

    setup_info_set_t(const setup_info_set_t &) = delete;
    setup_info_set_t &operator=(const setup_info_set_t &) = delete;

    ~setup_info_set_t() {
      if (set != INVALID_HANDLE_VALUE) {
        SetupDiDestroyDeviceInfoList(set);
      }
    }

    bool
    valid() const noexcept {
      return set != INVALID_HANDLE_VALUE;
    }

    HDEVINFO
    get() const noexcept {
      return set;
    }

  private:
    HDEVINFO set = INVALID_HANDLE_VALUE;
  };

  std::optional<std::wstring>
  find_device_path() {
    setup_info_set_t set {
      SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_ZAKO_INPUT_HID,
        nullptr,
        nullptr,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT)
    };
    if (!set.valid()) {
      return std::nullopt;
    }

    SP_DEVICE_INTERFACE_DATA iface {};
    iface.cbSize = sizeof(iface);
    if (!SetupDiEnumDeviceInterfaces(set.get(), nullptr, &GUID_DEVINTERFACE_ZAKO_INPUT_HID, 0, &iface)) {
      return std::nullopt;
    }

    DWORD required = 0;
    SetupDiGetDeviceInterfaceDetailW(set.get(), &iface, nullptr, 0, &required, nullptr);
    if (required == 0) {
      return std::nullopt;
    }

    auto buffer = std::make_unique<BYTE[]>(required);
    auto detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.get());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(set.get(), &iface, detail, required, nullptr, nullptr)) {
      return std::nullopt;
    }

    return std::wstring { detail->DevicePath };
  }

  template<class Payload>
  bool
  send_ioctl(HANDLE device, DWORD code, const Payload &payload) {
    DWORD bytes_returned = 0;
    return DeviceIoControl(
             device,
             code,
             const_cast<Payload *>(&payload),
             sizeof(payload),
             nullptr,
             0,
             &bytes_returned,
             nullptr) != FALSE;
  }

  bool
  is_modifier_vk(uint16_t vk, uint8_t &mask) {
    switch (vk) {
      case VK_SHIFT:
      case VK_LSHIFT:
        mask = 0x02;
        return true;
      case VK_RSHIFT:
        mask = 0x20;
        return true;
      case VK_CONTROL:
      case VK_LCONTROL:
        mask = 0x01;
        return true;
      case VK_RCONTROL:
        mask = 0x10;
        return true;
      case VK_MENU:
      case VK_LMENU:
        mask = 0x04;
        return true;
      case VK_RMENU:
        mask = 0x40;
        return true;
      case VK_LWIN:
        mask = 0x08;
        return true;
      case VK_RWIN:
        mask = 0x80;
        return true;
      default:
        return false;
    }
  }

  std::optional<uint8_t>
  vk_to_hid_usage(uint16_t vk) {
    if (vk >= 'A' && vk <= 'Z') {
      return static_cast<uint8_t>(0x04 + (vk - 'A'));
    }
    if (vk >= '1' && vk <= '9') {
      return static_cast<uint8_t>(0x1E + (vk - '1'));
    }

    switch (vk) {
      case '0': return 0x27;
      case VK_RETURN: return 0x28;
      case VK_ESCAPE: return 0x29;
      case VK_BACK: return 0x2A;
      case VK_TAB: return 0x2B;
      case VK_SPACE: return 0x2C;
      case VK_OEM_MINUS: return 0x2D;
      case VK_OEM_PLUS: return 0x2E;
      case VK_OEM_4: return 0x2F;
      case VK_OEM_6: return 0x30;
      case VK_OEM_5: return 0x31;
      case VK_OEM_1: return 0x33;
      case VK_OEM_7: return 0x34;
      case VK_OEM_3: return 0x35;
      case VK_OEM_COMMA: return 0x36;
      case VK_OEM_PERIOD: return 0x37;
      case VK_OEM_2: return 0x38;
      case VK_CAPITAL: return 0x39;
      case VK_F1: return 0x3A;
      case VK_F2: return 0x3B;
      case VK_F3: return 0x3C;
      case VK_F4: return 0x3D;
      case VK_F5: return 0x3E;
      case VK_F6: return 0x3F;
      case VK_F7: return 0x40;
      case VK_F8: return 0x41;
      case VK_F9: return 0x42;
      case VK_F10: return 0x43;
      case VK_F11: return 0x44;
      case VK_F12: return 0x45;
      case VK_SNAPSHOT: return 0x46;
      case VK_SCROLL: return 0x47;
      case VK_PAUSE: return 0x48;
      case VK_INSERT: return 0x49;
      case VK_HOME: return 0x4A;
      case VK_PRIOR: return 0x4B;
      case VK_DELETE: return 0x4C;
      case VK_END: return 0x4D;
      case VK_NEXT: return 0x4E;
      case VK_RIGHT: return 0x4F;
      case VK_LEFT: return 0x50;
      case VK_DOWN: return 0x51;
      case VK_UP: return 0x52;
      case VK_NUMLOCK: return 0x53;
      case VK_DIVIDE: return 0x54;
      case VK_MULTIPLY: return 0x55;
      case VK_SUBTRACT: return 0x56;
      case VK_ADD: return 0x57;
      case VK_DECIMAL: return 0x63;
      case VK_APPS: return 0x65;
      case VK_NUMPAD1: return 0x59;
      case VK_NUMPAD2: return 0x5A;
      case VK_NUMPAD3: return 0x5B;
      case VK_NUMPAD4: return 0x5C;
      case VK_NUMPAD5: return 0x5D;
      case VK_NUMPAD6: return 0x5E;
      case VK_NUMPAD7: return 0x5F;
      case VK_NUMPAD8: return 0x60;
      case VK_NUMPAD9: return 0x61;
      case VK_NUMPAD0: return 0x62;
      default:
        return std::nullopt;
    }
  }
}  // namespace

namespace platf {
  namespace input_hid {

    struct device_t::impl_t {
      HANDLE device = INVALID_HANDLE_VALUE;
      uint32_t sequence = 1;
      uint8_t mouse_buttons = 0;
      uint8_t keyboard_modifiers = 0;
      std::array<uint8_t, 6> keyboard_keys {};

      ~impl_t() {
        release_all();
        close();
      }

      bool
      open() {
        auto path = find_device_path();
        if (!path) {
          BOOST_LOG(info) << "zako-input-hid: device interface not found"sv;
          return false;
        }

        device = CreateFileW(
          path->c_str(),
          GENERIC_WRITE,
          FILE_SHARE_READ | FILE_SHARE_WRITE,
          nullptr,
          OPEN_EXISTING,
          FILE_ATTRIBUTE_NORMAL,
          nullptr);

        if (device == INVALID_HANDLE_VALUE) {
          BOOST_LOG(error) << "zako-input-hid: failed to open "sv
                           << wide_to_utf8(path->c_str())
                           << " error="sv << GetLastError();
          return false;
        }

        BOOST_LOG(info) << "zako-input-hid: connected to "sv << wide_to_utf8(path->c_str());
        return true;
      }

      void
      close() {
        if (device != INVALID_HANDLE_VALUE) {
          CloseHandle(device);
          device = INVALID_HANDLE_VALUE;
        }
      }

      bool
      available() const {
        return device != INVALID_HANDLE_VALUE;
      }

      uint32_t
      next_sequence() {
        return sequence++;
      }

      bool
      submit_mouse(int16_t dx, int16_t dy, int8_t wheel_v, int8_t wheel_h) {
        if (!available()) {
          return false;
        }

        const mouse_submit_t payload {
          next_sequence(),
          {
            ZAKO_HID_MOUSE_REPORT_ID,
            mouse_buttons,
            dx,
            dy,
            wheel_v,
            wheel_h,
          },
        };

        if (!send_ioctl(device, IOCTL_ZAKO_INPUT_HID_SUBMIT_MOUSE, payload)) {
          BOOST_LOG(error) << "zako-input-hid: mouse submit failed error="sv << GetLastError();
          return false;
        }

        return true;
      }

      bool
      submit_keyboard() {
        if (!available()) {
          return false;
        }

        keyboard_submit_t payload {
          next_sequence(),
          {
            ZAKO_HID_KEYBOARD_REPORT_ID,
            keyboard_modifiers,
            0,
            {},
          },
        };
        std::memcpy(payload.report.keys, keyboard_keys.data(), keyboard_keys.size());

        if (!send_ioctl(device, IOCTL_ZAKO_INPUT_HID_SUBMIT_KEYBOARD, payload)) {
          BOOST_LOG(error) << "zako-input-hid: keyboard submit failed error="sv << GetLastError();
          return false;
        }

        return true;
      }

      bool
      release_all() {
        mouse_buttons = 0;
        keyboard_modifiers = 0;
        keyboard_keys.fill(0);

        if (!available()) {
          return false;
        }

        const release_all_t payload {
          next_sequence(),
          0,
        };

        return send_ioctl(device, IOCTL_ZAKO_INPUT_HID_RELEASE_ALL, payload);
      }

      void
      set_usage(uint8_t usage, bool release) {
        auto it = std::find(keyboard_keys.begin(), keyboard_keys.end(), usage);
        if (release) {
          if (it != keyboard_keys.end()) {
            *it = 0;
          }
          return;
        }

        if (it != keyboard_keys.end()) {
          return;
        }

        auto empty = std::find(keyboard_keys.begin(), keyboard_keys.end(), 0);
        if (empty != keyboard_keys.end()) {
          *empty = usage;
        }
        else {
          BOOST_LOG(warning) << "zako-input-hid: keyboard rollover limit reached"sv;
        }
      }
    };

    device_t::device_t():
        impl(std::make_unique<impl_t>()) {}
    device_t::~device_t() = default;
    device_t::device_t(device_t &&other) noexcept = default;
    device_t &device_t::operator=(device_t &&other) noexcept = default;

    bool
    device_t::is_available() const {
      return impl && impl->available();
    }

    bool
    device_t::move(int16_t delta_x, int16_t delta_y) {
      return impl->submit_mouse(delta_x, delta_y, 0, 0);
    }

    bool
    device_t::button(uint8_t button_mask, bool release) {
      impl->mouse_buttons = release ?
                              static_cast<uint8_t>(impl->mouse_buttons & ~button_mask) :
                              static_cast<uint8_t>(impl->mouse_buttons | button_mask);
      return impl->submit_mouse(0, 0, 0, 0);
    }

    bool
    device_t::scroll(int8_t distance) {
      return impl->submit_mouse(0, 0, distance, 0);
    }

    bool
    device_t::hscroll(int8_t distance) {
      return impl->submit_mouse(0, 0, 0, distance);
    }

    bool
    device_t::keyboard_update(uint16_t vk, bool release, uint8_t) {
      uint8_t modifier_mask = 0;
      if (is_modifier_vk(vk, modifier_mask)) {
        impl->keyboard_modifiers = release ?
                                     static_cast<uint8_t>(impl->keyboard_modifiers & ~modifier_mask) :
                                     static_cast<uint8_t>(impl->keyboard_modifiers | modifier_mask);
        return impl->submit_keyboard();
      }

      auto usage = vk_to_hid_usage(vk);
      if (!usage) {
        BOOST_LOG(debug) << "zako-input-hid: unsupported VK for HID keyboard report vk=0x"sv
                         << std::hex << vk;
        return false;
      }

      impl->set_usage(*usage, release);
      return impl->submit_keyboard();
    }

    bool
    device_t::release_all() {
      return impl->release_all();
    }

    device_t
    create() {
      device_t dev;
      dev.impl->open();
      return dev;
    }

  }  // namespace input_hid
}  // namespace platf
