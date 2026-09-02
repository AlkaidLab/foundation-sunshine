/**
 * @file src/platform/windows/virtual_touchscreen_session.cpp
 * @brief Windows implementation: device + loopback bridge + usbip attach.
 *
 * Attach requires elevation to open the VHCI device interface; the Sunshine
 * service already runs as SYSTEM so no extra prompt is needed in-stream.
 * On detach the VHCI removes the emulated device automatically when the
 * control connection drops.
 */

#include "src/virtual_touchscreen_session.h"

#include "src/logging.h"
#include "src/remote_usb/loopback_usbip_bridge.h"
#include "src/remote_usb/virtual_touchscreen_device.h"

#include <Windows.h>

#include <boost/system/error_code.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif


namespace vts {

  namespace {

    namespace rusb = remote_usb;

    struct session_state_t {
      std::mutex mutex;

      // Construction order matters: the device outlives the bridge (the
      // bridge's reply hook calls into the device), and both are addressed
      // through this stable allocation for the whole session.
      rusb::virtual_touchscreen_device device;
      rusb::loopback_usbip_bridge bridge;
      std::wstring usbip_executable;
      std::wstring busid;
      std::map<std::uint8_t, rusb::touchscreen_contact> active;
      bool imported { false };

      explicit session_state_t(rusb::virtual_touchscreen_device::config cfg) :
          device(cfg) {}
    };

    std::mutex state_mutex;
    std::unique_ptr<session_state_t> state;

    std::wstring
    to_wide(const std::string &utf8) {
      if (utf8.empty()) {
        return {};
      }
      int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int) utf8.size(), nullptr, 0);
      std::wstring out(size, L'\0');
      MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int) utf8.size(), out.data(), size);
      return out;
    }

    /** Run usbip.exe with output captured; killed after 20 s on hang. */
    std::string
    run_usbip(const std::wstring &cmdline) {
      SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
      HANDLE read_end = nullptr, write_end = nullptr;
      if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        return "pipe failed";
      }
      SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
      STARTUPINFOW si = {};
      si.cb = sizeof(si);
      si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
      si.wShowWindow = SW_HIDE;
      si.hStdOutput = write_end;
      si.hStdError = write_end;
      PROCESS_INFORMATION pi = {};
      std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
      mutable_cmd.push_back(L'\0');
      if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &si, &pi)) {
        CloseHandle(read_end);
        CloseHandle(write_end);
        return "create process failed: " + std::to_string(GetLastError());
      }
      CloseHandle(write_end);
      std::string out;
      std::thread reader([&]() {
        char buf[512];
        DWORD read = 0;
        while (ReadFile(read_end, buf, sizeof(buf), &read, nullptr) && read != 0) {
          out.append(buf, read);
        }
      });
      const bool timed_out = WaitForSingleObject(pi.hProcess, 20000) == WAIT_TIMEOUT;
      if (timed_out) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 3000);
      }
      reader.join();
      DWORD code = 0;
      GetExitCodeProcess(pi.hProcess, &code);
      CloseHandle(read_end);
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
      return out + (timed_out ? " [timed out]" : " [exit " + std::to_string(code) + "]");
    }

  }  // namespace

  bool
  start(const session_config &config) {
    std::lock_guard<std::mutex> state_lock(state_mutex);
    stop();

    rusb::virtual_touchscreen_device::config cfg;
    cfg.width_px = config.width_px;
    cfg.height_px = config.height_px;

    state = std::make_unique<session_state_t>(cfg);
    auto *raw = state.get();  // stable for the whole session
    raw->usbip_executable = config.usbip_executable.empty()
                              ? L"usbip.exe"
                              : to_wide(config.usbip_executable);
    raw->busid.clear();

    auto callbacks = rusb::callbacks {};
    // The device object address is stable; the bridge serialises requests.
    callbacks.on_request = [raw](std::vector<std::uint8_t> pdu) {
      return raw->device.handle_request(std::move(pdu));
    };
    callbacks.on_imported = []() {
      BOOST_LOG(info) << "vts: VHCI imported the virtual touchscreen";
    };
    callbacks.on_closed = [](rusb::close_reason reason) {
      BOOST_LOG(info) << "vts: bridge closed (" << (int) reason << ")";
    };

    boost::system::error_code ec;
    auto endpoint = raw->bridge.start(raw->device.info(), std::move(callbacks), ec);
    if (!endpoint) {
      BOOST_LOG(warning) << "vts: bridge start failed: " << ec.message();
      state.reset();
      return false;
    }
    raw->device.set_send_reply([raw](std::vector<std::uint8_t> wire) {
      raw->bridge.send_reply(std::move(wire));
    });

    std::wstring cmd = L"\"" + raw->usbip_executable + L"\" --tcp-port " +
                       std::to_wstring(endpoint->port) + L" attach --remote " +
                       to_wide(endpoint->address) + L" --bus-id " + to_wide(endpoint->busid) +
                       L" --terse";
    const std::string attach_out = run_usbip(cmd);
    BOOST_LOG(info) << "vts: usbip attach: " << attach_out;

    for (int i = 0; i < 60 && !raw->device.imported(); ++i) {
      Sleep(100);
    }
    if (!raw->device.imported()) {
      BOOST_LOG(warning) << "vts: device was not imported; tearing down";
      raw->bridge.stop();
      state.reset();
      return false;
    }

    BOOST_LOG(info) << "vts: virtual touchscreen attached (" << config.width_px << "x"
                    << config.height_px << ")";
    return true;
  }

  void
  update_contact(std::uint8_t contact_id, std::uint16_t x, std::uint16_t y, bool tip) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (!state) {
      return;
    }
    std::lock_guard<std::mutex> device_lock(state->mutex);
    if (tip) {
      state->active[contact_id] = rusb::touchscreen_contact {
        contact_id, x, y, 0, true, true
      };
    }
    else {
      state->active.erase(contact_id);
    }
    std::vector<rusb::touchscreen_contact> contacts;
    contacts.reserve(state->active.size());
    for (const auto &[id, c] : state->active) {
      contacts.push_back(c);
    }
    state->device.update_contacts(contacts);
  }

  void
  lift_contact(std::uint8_t contact_id) {
    update_contact(contact_id, 0, 0, false);
  }

  void
  stop() {
    std::lock_guard<std::mutex> state_lock(state_mutex);
    if (!state) {
      return;
    }
    state->bridge.stop();
    BOOST_LOG(info) << "vts: virtual touchscreen detached";
    state.reset();
  }

}  // namespace vts
