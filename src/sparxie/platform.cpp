#include "src/sparxie/platform.h"
#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <algorithm>
  #include <chrono>
  #include <cstring>
  #include <cwctype>
  #include <set>
  #include <thread>
  #include <objbase.h>
  #include <shlobj.h>
  #include <tlhelp32.h>
  #include <vector>
  #include <wincodec.h>
  #include <windows.h>
  #include <winhttp.h>
namespace sparxie {
  namespace {
    std::wstring
    widen(std::string_view text) {
      if (text.empty()) return {};
      const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
      std::wstring out(size > 0 ? static_cast<std::size_t>(size) : 0, wchar_t(0));
      if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
      return out;
    }
    std::string
    narrow(std::wstring_view text) {
      if (text.empty()) return {};
      const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
      std::string out(size > 0 ? static_cast<std::size_t>(size) : 0, char(0));
      if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
      return out;
    }
    // The hive of the person the host works for. Opened while impersonating that person, so a service
    // running as SYSTEM reads their HoYoPlay and game keys and not its own.
    struct UserHive {
      HKEY key = nullptr;
      UserHive() {
        host_as_user([this] {
          if (RegOpenCurrentUser(KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS) key = nullptr;
        });
      }
      ~UserHive() {
        if (key) RegCloseKey(key);
      }
      UserHive(const UserHive &) = delete;
      UserHive &
      operator=(const UserHive &) = delete;
      bool
      ok() const { return key != nullptr; }
    };
    class WinRegistry: public Registry {
    public:
      RegValue
      get(std::string_view key, std::string_view name) const override {
        const std::wstring wkey = widen(key);
        const std::wstring wname = widen(name);
        DWORD type = 0;
        DWORD bytes = 0;
        const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_DWORD | RRF_RT_REG_BINARY;
        UserHive hive;
        if (!hive.ok()) return {};
        if (RegGetValueW(hive.key, wkey.c_str(), wname.c_str(), flags, &type, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0) return {};
        std::vector<std::uint8_t> buffer(bytes);
        if (RegGetValueW(hive.key, wkey.c_str(), wname.c_str(), flags, &type, buffer.data(), &bytes) != ERROR_SUCCESS) return {};
        RegValue value;
        if (type == REG_DWORD && bytes >= sizeof(DWORD)) {
          value.kind = RegValue::Kind::dword;
          std::memcpy(&value.dword, buffer.data(), sizeof(DWORD));
        }
        else if (type == REG_SZ || type == REG_EXPAND_SZ) {
          value.kind = RegValue::Kind::text;
          const auto chars = bytes / sizeof(wchar_t);
          value.text = narrow(std::wstring_view(reinterpret_cast<const wchar_t *>(buffer.data()), chars));
          while (!value.text.empty() && value.text.back() == char(0)) value.text.pop_back();
        }
        else if (type == REG_BINARY) {
          value.kind = RegValue::Kind::binary;
          value.bytes.assign(buffer.begin(), buffer.begin() + bytes);
        }
        return value;
      }
      std::vector<std::string>
      subkeys(std::string_view key) const override {
        std::vector<std::string> names;
        UserHive hive;
        if (!hive.ok()) return names;
        HKEY handle = nullptr;
        if (RegOpenKeyExW(hive.key, widen(key).c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) return names;
        DWORD index = 0;
        wchar_t name[256] {};
        DWORD size = 256;
        while (RegEnumKeyExW(handle, index++, name, &size, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
          names.push_back(narrow(name));
          size = 256;
        }
        RegCloseKey(handle);
        return names;
      }
      bool
      set_dword(std::string_view key, std::string_view name, std::uint32_t stored) override {
        UserHive hive;
        if (!hive.ok()) return false;
        HKEY handle = nullptr;
        if (RegCreateKeyExW(hive.key, widen(key).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS) return false;
        const DWORD value = stored;
        const LONG rc = RegSetValueExW(handle, widen(name).c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
        RegCloseKey(handle);
        return rc == ERROR_SUCCESS;
      }
      bool
      set_binary(std::string_view key, std::string_view name, const std::vector<std::uint8_t> &bytes) override {
        UserHive hive;
        if (!hive.ok()) return false;
        HKEY handle = nullptr;
        if (RegCreateKeyExW(hive.key, widen(key).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS) return false;
        const LONG rc = RegSetValueExW(handle, widen(name).c_str(), 0, REG_BINARY, bytes.empty() ? nullptr : bytes.data(), static_cast<DWORD>(bytes.size()));
        RegCloseKey(handle);
        return rc == ERROR_SUCCESS;
      }
      bool
      remove(std::string_view key, std::string_view name) override {
        UserHive hive;
        if (!hive.ok()) return false;
        HKEY handle = nullptr;
        if (RegOpenKeyExW(hive.key, widen(key).c_str(), 0, KEY_SET_VALUE, &handle) != ERROR_SUCCESS) return false;
        const LONG rc = RegDeleteValueW(handle, widen(name).c_str());
        RegCloseKey(handle);
        return rc == ERROR_SUCCESS;
      }
      bool
      set_text(std::string_view key, std::string_view name, std::string_view text) override {
        UserHive hive;
        if (!hive.ok()) return false;
        HKEY handle = nullptr;
        if (RegCreateKeyExW(hive.key, widen(key).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS) return false;
        const std::wstring wide = widen(text);
        const auto *bytes = reinterpret_cast<const BYTE *>(wide.c_str());
        const DWORD size = static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t));
        const LONG rc = RegSetValueExW(handle, widen(name).c_str(), 0, REG_SZ, bytes, size);
        RegCloseKey(handle);
        return rc == ERROR_SUCCESS;
      }
    };
    struct ComInit {
      bool own = false;
      ComInit() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        own = SUCCEEDED(hr);
      }
      ~ComInit() {
        if (own) CoUninitialize();
      }
    };
  }  // namespace
  Registry &
  host_registry() {
    static WinRegistry registry;
    return registry;
  }
  bool
  host_process_running(const std::vector<std::string> &names) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
      do {
        const std::string exe = narrow(entry.szExeFile);
        for (const auto &name : names)
          if (ascii_lower(exe) == ascii_lower(name)) found = true;
      } while (!found && Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
  }
  int
  host_raise_game_priority(const fs::path &exe, std::chrono::milliseconds wait) {
    // the same file, whatever the slashes and the case
    auto key = [](std::wstring text) {
      for (auto &c : text) {
        if (c == L'/') c = L'\\';
        c = static_cast<wchar_t>(std::towlower(c));
      }
      return text;
    };
    const std::wstring want = key(exe.wstring());
    const std::wstring name = key(exe.filename().wstring());
    if (want.empty() || name.empty()) return 0;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + wait;
    std::chrono::steady_clock::time_point stop = deadline;
    std::set<DWORD> raised;
    for (;;) {
      const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
      if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry {};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
          do {
            if (key(entry.szExeFile) != name) continue;
            const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) continue;
            wchar_t image[MAX_PATH * 2] = {};
            DWORD size = static_cast<DWORD>(sizeof(image) / sizeof(image[0]));
            if (QueryFullProcessImageNameW(process, 0, image, &size) && key(std::wstring(image, size)) == want) {
              const DWORD current = GetPriorityClass(process);
              if (current == NORMAL_PRIORITY_CLASS || current == BELOW_NORMAL_PRIORITY_CLASS || current == IDLE_PRIORITY_CLASS) {
                if (SetPriorityClass(process, ABOVE_NORMAL_PRIORITY_CLASS)) raised.insert(entry.th32ProcessID);
              }
              else if (current == ABOVE_NORMAL_PRIORITY_CLASS || current == HIGH_PRIORITY_CLASS) {
                raised.insert(entry.th32ProcessID);
              }
              if (!raised.empty() && stop == deadline) stop = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(20));
            }
            CloseHandle(process);
          } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
      }
      if (std::chrono::steady_clock::now() >= stop) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return static_cast<int>(raised.size());
  }
  std::vector<BrowseEntry>
  host_list_roots() {
    std::vector<BrowseEntry> roots;
    // Mapped network drives belong to the person, so the list is read as them.
    host_as_user([&] {
      const DWORD mask = GetLogicalDrives();
      for (int bit = 0; bit < 26; ++bit) {
        if (!(mask & (1u << bit))) continue;
        const std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + bit)) + L":\\";
        const UINT type = GetDriveTypeW(root.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_REMOTE && type != DRIVE_CDROM) continue;
        wchar_t label[MAX_PATH] = {};
        GetVolumeInformationW(root.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        const std::string letter = narrow(root.substr(0, 2));
        const std::string text = narrow(label);
        roots.push_back({ text.empty() ? letter : text + " (" + letter + ")", narrow(root), "drive", false });
      }
    });
    return roots;
  }
  namespace {
    // One HTTPS round trip of the art refresh. Bounded in every way a launcher's server could stall or flood it: timeouts (resolve, connect, send, receive), HTTPS
    // only, the redirect limit of WinHTTP, a 200 answer only, gzip/deflate answered decoded (Kuro's index files are gzip) and at most kMaxBody bytes.
    constexpr std::size_t kMaxBody = 32u * 1024u * 1024u;

    std::optional<std::string>
    http_request(const wchar_t *method, const std::string &url, const std::string *json_body) {
      const std::wstring wide = widen(url);
      URL_COMPONENTS parts {};
      parts.dwStructSize = sizeof(parts);
      wchar_t host[256] {};
      wchar_t path[4096] {};
      parts.lpszHostName = host;
      parts.dwHostNameLength = 255;
      parts.lpszUrlPath = path;
      parts.dwUrlPathLength = 4095;
      if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) return std::nullopt;
      const HINTERNET session = WinHttpOpen(L"foundation-art/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
      if (!session) return std::nullopt;
      WinHttpSetTimeouts(session, 8000, 8000, 15000, 20000);
      const HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
      const HINTERNET request = connect ? WinHttpOpenRequest(connect, method, path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
      std::string body;
      bool ok = false;
      if (request) {
        DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
        const wchar_t json_headers[] = L"Content-Type: application/json\r\n";
        if (json_body)
          ok = WinHttpSendRequest(request, json_headers, static_cast<DWORD>(-1), const_cast<char *>(json_body->data()), static_cast<DWORD>(json_body->size()), static_cast<DWORD>(json_body->size()), 0);
        else
          ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        ok = ok && WinHttpReceiveResponse(request, nullptr);
      }
      if (ok) {
        DWORD status = 0;
        DWORD size = sizeof(status);
        ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) && status == 200;
      }
      if (ok) {
        DWORD available = 0;
        do {
          if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
          if (body.size() + available > kMaxBody) {
            ok = false;
            break;
          }
          const std::size_t at = body.size();
          body.resize(at + available);
          DWORD read = 0;
          if (!WinHttpReadData(request, body.data() + at, available, &read)) {
            body.resize(at);
            ok = false;
            break;
          }
          body.resize(at + read);
        } while (available > 0);
      }
      if (request) WinHttpCloseHandle(request);
      if (connect) WinHttpCloseHandle(connect);
      WinHttpCloseHandle(session);
      if (!ok) return std::nullopt;
      return body;
    }
  }  // namespace

  std::optional<std::string>
  host_http_get(const std::string &url) {
    return http_request(L"GET", url, nullptr);
  }

  std::optional<std::string>
  host_http_post_json(const std::string &url, const std::string &json_body) {
    return http_request(L"POST", url, &json_body);
  }
  Rgba
  host_decode_image(std::string_view bytes) {
    Rgba image;
    if (bytes.empty()) return image;
    ComInit com;
    IWICImagingFactory *factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return image;
    IWICStream *stream = nullptr;
    if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE *>(const_cast<char *>(bytes.data())), static_cast<DWORD>(bytes.size())))) {
      if (stream) stream->Release();
      factory->Release();
      return image;
    }
    IWICBitmapDecoder *decoder = nullptr;
    factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    IWICBitmapFrameDecode *frame = nullptr;
    if (decoder) decoder->GetFrame(0, &frame);
    IWICFormatConverter *converter = nullptr;
    factory->CreateFormatConverter(&converter);
    if (frame && converter && SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom))) {
      UINT width = 0, height = 0;
      converter->GetSize(&width, &height);
      image.w = static_cast<int>(width);
      image.h = static_cast<int>(height);
      image.px.resize(static_cast<std::size_t>(width) * height * 4);
      // a codec that cannot deliver the pixels (a WebP codec that fails half way) is a failed decode, never a picture of zeros
      if (image.px.empty() || FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(image.px.size()), image.px.data()))) image = Rgba {};
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    stream->Release();
    factory->Release();
    return image;
  }
  fs::path
  host_program_data() {
    // The documented folder id, not a hard-coded path: the drive can differ.
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &path)) || !path) return {};
    std::wstring text(path);
    CoTaskMemFree(path);
    return fs::path(text);
  }

  std::vector<fs::path>
  host_uninstall_locations(const std::vector<std::string> &key_names) {
    std::vector<fs::path> out;
    static const wchar_t *value = L"InstallLocation";
    for (const std::string &name : key_names) {
      const std::wstring key = widen("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") + widen(name);
      // A machine-wide launcher install writes HKLM (the launcher is a 32-bit
      // program, so its key sits in the 32-bit view), a per-user install
      // writes the signed-in person's HKCU.
      UserHive hive;
      const struct {
        HKEY root;
        DWORD view;
      } views[] = {
        { HKEY_LOCAL_MACHINE, RRF_SUBKEY_WOW6432KEY },
        { HKEY_LOCAL_MACHINE, RRF_SUBKEY_WOW6464KEY },
        { hive.ok() ? hive.key : nullptr, 0 },
      };
      for (const auto &candidate : views) {
        if (!candidate.root) continue;
        wchar_t text[MAX_PATH] {};
        DWORD bytes = sizeof(text);
        if (RegGetValueW(candidate.root, key.c_str(), value, RRF_RT_REG_SZ | candidate.view, nullptr, text, &bytes) == ERROR_SUCCESS && text[0]) {
          out.push_back(fs::path(narrow(text)));
          break;
        }
      }
    }
    return out;
  }
}  // namespace sparxie
#else
namespace sparxie {
  Registry &
  host_registry() {
    static MemoryRegistry registry;
    return registry;
  }
  bool
  host_process_running(const std::vector<std::string> &) { return false; }
  int
  host_raise_game_priority(const fs::path &, std::chrono::milliseconds) { return 0; }
  std::vector<BrowseEntry>
  host_list_roots() { return { { "/", "/", "drive", false } }; }
  std::optional<std::string>
  host_http_get(const std::string &) { return std::nullopt; }
  std::optional<std::string>
  host_http_post_json(const std::string &, const std::string &) { return std::nullopt; }
  Rgba
  host_decode_image(std::string_view) { return {}; }
  fs::path
  host_program_data() { return {}; }
  std::vector<fs::path>
  host_uninstall_locations(const std::vector<std::string> &) { return {}; }
}  // namespace sparxie
#endif
namespace sparxie {
  namespace {
    UserRunner &
    user_runner() {
      static UserRunner runner;
      return runner;
    }
  }  // namespace
  void
  set_user_runner(UserRunner runner) { user_runner() = std::move(runner); }
  bool
  host_as_user(const UserCall &call) {
    if (UserRunner &runner = user_runner(); runner) return runner(call);
    call();
    return true;
  }
}  // namespace sparxie
