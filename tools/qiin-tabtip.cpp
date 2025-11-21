/**
 * @file tools/qiin-tabtip.cpp
 * @brief 调出或隐藏 Windows 触摸虚拟键盘的工具
 * @note 优化版本 - 不使用 C++ 标准库以减小文件大小
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <initguid.h>
#include <Objbase.h>

// 简单的控制台输出函数（替代 iostream）
static void Print(const wchar_t* msg) {
  DWORD written;
  HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
  WriteConsoleW(hConsole, msg, lstrlenW(msg), &written, NULL);
  WriteConsoleW(hConsole, L"\r\n", 2, &written, NULL);
}

static void PrintError(const wchar_t* msg) {
  DWORD written;
  HANDLE hConsole = GetStdHandle(STD_ERROR_HANDLE);
  WriteConsoleW(hConsole, msg, lstrlenW(msg), &written, NULL);
  WriteConsoleW(hConsole, L"\r\n", 2, &written, NULL);
}

// 字符串比较（不区分大小写）
static bool StrEqualI(const wchar_t* str1, const wchar_t* str2) {
  return lstrcmpiW(str1, str2) == 0;
}

// ITipInvocation COM 接口 - 这是微软官方的触摸键盘 API
// CLSID for UIHostNoLaunch
DEFINE_GUID(CLSID_UIHostNoLaunch,
    0x4CE576FA, 0x83DC, 0x4f88, 0x95, 0x1C, 0x9D, 0x07, 0x82, 0xB4, 0xE3, 0x76);

// IID for ITipInvocation
DEFINE_GUID(IID_ITipInvocation,
    0x37c994e7, 0x432b, 0x4834, 0xa2, 0xf7, 0xdc, 0xe1, 0xf1, 0x3b, 0x83, 0x4b);

// ITipInvocation 接口定义
struct ITipInvocation : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Toggle(HWND wnd) = 0;
};

// Windows 10/11 触摸键盘路径
const wchar_t* TABTIP_PATH = L"C:\\Program Files\\Common Files\\microsoft shared\\ink\\TabTip.exe";

/**
 * 检查触摸键盘是否正在运行
 */
bool IsKeyboardVisible() {
  HWND hwnd = FindWindow(L"IPTip_Main_Window", NULL);
  if (hwnd == NULL) {
    // Windows 11 可能使用不同的类名
    hwnd = FindWindow(L"ApplicationFrameWindow", L"Microsoft Text Input Application");
  }
  
  if (hwnd != NULL) {
    // 检查窗口是否可见
    return IsWindowVisible(hwnd);
  }
  return false;
}

/**
 * 检查 TabTip.exe 是否存在
 */
bool CheckTabTipExists() {
  DWORD dwAttrib = GetFileAttributes(TABTIP_PATH);
  return (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

/**
 * 启用触摸键盘的桌面模式自动调用
 * 这是 Windows 10/11 中显示 TabTip 的关键设置
 */
bool EnableDesktopModeAutoInvoke() {
  HKEY hKey;
  LONG result = RegOpenKeyEx(
    HKEY_CURRENT_USER,
    L"SOFTWARE\\Microsoft\\TabletTip\\1.7",
    0,
    KEY_READ | KEY_WRITE,
    &hKey
  );

  if (result != ERROR_SUCCESS) {
    // 如果键不存在，尝试创建
    result = RegCreateKeyEx(
      HKEY_CURRENT_USER,
      L"SOFTWARE\\Microsoft\\TabletTip\\1.7",
      0,
      NULL,
      REG_OPTION_NON_VOLATILE,
      KEY_READ | KEY_WRITE,
      NULL,
      &hKey,
      NULL
    );
    
    if (result != ERROR_SUCCESS) {
      return false;
    }
  }

  // 设置 EnableDesktopModeAutoInvoke 为 1
  DWORD value = 1;
  result = RegSetValueEx(
    hKey,
    L"EnableDesktopModeAutoInvoke",
    0,
    REG_DWORD,
    (BYTE*)&value,
    sizeof(DWORD)
  );

  RegCloseKey(hKey);
  return result == ERROR_SUCCESS;
}

/**
 * 检查是否已启用桌面模式自动调用
 */
bool IsDesktopModeAutoInvokeEnabled() {
  HKEY hKey;
  LONG result = RegOpenKeyEx(
    HKEY_CURRENT_USER,
    L"SOFTWARE\\Microsoft\\TabletTip\\1.7",
    0,
    KEY_READ,
    &hKey
  );

  if (result != ERROR_SUCCESS) {
    return false;
  }

  DWORD value = 0;
  DWORD size = sizeof(DWORD);
  result = RegQueryValueEx(
    hKey,
    L"EnableDesktopModeAutoInvoke",
    NULL,
    NULL,
    (BYTE*)&value,
    &size
  );

  RegCloseKey(hKey);
  return (result == ERROR_SUCCESS && value == 1);
}

/**
 * 强制显示已存在的键盘窗口
 */
bool ForceShowKeyboardWindow() {
  // 查找键盘窗口
  HWND hwnd = FindWindow(L"IPTip_Main_Window", NULL);
  
  if (hwnd == NULL) {
    // Windows 11 可能使用不同的类名
    hwnd = FindWindow(L"ApplicationFrameWindow", L"Microsoft Text Input Application");
  }

  if (hwnd != NULL) {
    // 显示窗口
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    
    // 确保窗口位置在屏幕内
    RECT rect;
    GetWindowRect(hwnd, &rect);
    int keyboardHeight = rect.bottom - rect.top;
    
    if (keyboardHeight > 0) {
      int screenWidth = GetSystemMetrics(SM_CXSCREEN);
      int screenHeight = GetSystemMetrics(SM_CYSCREEN);
      int keyboardWidth = rect.right - rect.left;
      int x = (screenWidth - keyboardWidth) / 2;
      int y = screenHeight - keyboardHeight - 50;
      
      SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
      Sleep(50);
      SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
    
    return IsWindowVisible(hwnd);
  }
  
  return false;
}

/**
 * 使用 COM 接口显示触摸键盘（推荐方法）
 * 这是 Microsoft 官方的 ITipInvocation 接口
 */
bool ShowKeyboardViaCOM() {
  HRESULT hr = CoInitialize(NULL);
  bool needsUninit = SUCCEEDED(hr);
  
  ITipInvocation* pTipInvocation = NULL;
  hr = CoCreateInstance(
    CLSID_UIHostNoLaunch,
    NULL,
    CLSCTX_INPROC_HANDLER | CLSCTX_LOCAL_SERVER,
    IID_ITipInvocation,
    (void**)&pTipInvocation
  );
  
  bool success = false;
  if (SUCCEEDED(hr) && pTipInvocation) {
    hr = pTipInvocation->Toggle(GetDesktopWindow());
    success = SUCCEEDED(hr);
    pTipInvocation->Release();
  }
  
  if (needsUninit) {
    CoUninitialize();
  }
  
  return success;
}

/**
 * 显示触摸键盘（综合方法）
 */
bool ShowKeyboard() {
  // 方法 1: 使用 COM 接口（最可靠的方法）
  if (ShowKeyboardViaCOM()) {
    Print(L"✓ 触摸键盘已显示");
    return true;
  }
  
  // 方法 2: 传统方法作为备选
  if (!CheckTabTipExists()) {
    PrintError(L"✗ 找不到 TabTip.exe");
    return false;
  }

  // 确保注册表设置正确
  if (!IsDesktopModeAutoInvokeEnabled()) {
    EnableDesktopModeAutoInvoke();
  }

  // 检查窗口是否已存在
  HWND existingWnd = FindWindow(L"IPTip_Main_Window", NULL);
  if (existingWnd == NULL) {
    existingWnd = FindWindow(L"ApplicationFrameWindow", L"Microsoft Text Input Application");
  }

  if (existingWnd != NULL) {
    if (ForceShowKeyboardWindow()) {
      Print(L"✓ 触摸键盘已显示");
      return true;
    }
  }

  // 启动 TabTip.exe
  SHELLEXECUTEINFO sei = { 0 };
  sei.cbSize = sizeof(SHELLEXECUTEINFO);
  sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
  sei.lpVerb = L"open";
  sei.lpFile = TABTIP_PATH;
  sei.nShow = SW_SHOW;

  if (ShellExecuteEx(&sei)) {
    if (sei.hProcess) {
      WaitForSingleObject(sei.hProcess, 1000);
      CloseHandle(sei.hProcess);
    }
    Sleep(500);
    
    if (ForceShowKeyboardWindow()) {
      Print(L"✓ 触摸键盘已显示");
      return true;
    }
  }

  // 最后备选：OSK
  HINSTANCE result = ShellExecute(NULL, L"open", L"osk.exe", NULL, NULL, SW_SHOW);
  if ((INT_PTR)result > 32) {
    Print(L"✓ 屏幕键盘已显示");
    return true;
  }
  
  PrintError(L"✗ 无法显示键盘");
  return false;
}

/**
 * 隐藏触摸键盘
 */
bool HideKeyboard() {
  // 查找键盘窗口
  HWND hwnd = FindWindow(L"IPTip_Main_Window", NULL);
  if (hwnd == NULL) {
    // Windows 11 可能使用不同的类名
    hwnd = FindWindow(L"ApplicationFrameWindow", L"Microsoft Text Input Application");
  }

  if (hwnd != NULL && IsWindowVisible(hwnd)) {
    PostMessage(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
    Print(L"✓ 触摸键盘已隐藏");
    return true;
  }
  
  Print(L"触摸键盘未运行或已隐藏");
  return false;
}

/**
 * 切换触摸键盘状态
 */
bool ToggleKeyboard() {
  if (IsKeyboardVisible()) {
    return HideKeyboard();
  } else {
    return ShowKeyboard();
  }
}

/**
 * 诊断系统环境
 */
void Diagnose() {
  wchar_t buffer[256];
  
  Print(L"=== 系统诊断信息 ===");
  Print(L"");
  
  // 检查 Windows 版本
  OSVERSIONINFOEX osvi = { 0 };
  osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
  #if defined(_MSC_VER)
  #pragma warning(push)
  #pragma warning(disable: 4996)
  #endif
  GetVersionEx((LPOSVERSIONINFO)&osvi);
  #if defined(_MSC_VER)
  #pragma warning(pop)
  #endif
  wsprintfW(buffer, L"Windows 版本: %d.%d", osvi.dwMajorVersion, osvi.dwMinorVersion);
  Print(buffer);
  
  // 检查 TabTip.exe
  Print(L"");
  wsprintfW(buffer, L"TabTip 路径: %s", TABTIP_PATH);
  Print(buffer);
  Print(CheckTabTipExists() ? L"TabTip.exe: ✓ 存在" : L"TabTip.exe: ✗ 不存在");
  
  // 检查注册表设置
  Print(L"");
  Print(L"注册表设置:");
  Print(IsDesktopModeAutoInvokeEnabled() ? 
        L"  EnableDesktopModeAutoInvoke: ✓ 已启用" : 
        L"  EnableDesktopModeAutoInvoke: ✗ 未启用");
  
  // 检查键盘窗口
  Print(L"");
  Print(L"检查键盘窗口:");
  HWND hwnd = FindWindow(L"IPTip_Main_Window", NULL);
  if (hwnd) {
    Print(L"  IPTip_Main_Window: ✓ 找到 (Windows 10)");
    Print(IsWindowVisible(hwnd) ? L"  可见性: 可见" : L"  可见性: 隐藏");
  } else {
    Print(L"  IPTip_Main_Window: ✗ 未找到");
  }
  
  hwnd = FindWindow(L"ApplicationFrameWindow", L"Microsoft Text Input Application");
  if (hwnd) {
    Print(L"  ApplicationFrameWindow: ✓ 找到 (Windows 11)");
    Print(IsWindowVisible(hwnd) ? L"  可见性: 可见" : L"  可见性: 隐藏");
  } else {
    Print(L"  ApplicationFrameWindow: ✗ 未找到");
  }
  
  Print(L"");
  Print(IsKeyboardVisible() ? L"当前键盘状态: 可见" : L"当前键盘状态: 隐藏");
}

/**
 * 显示屏幕键盘 (OSK)
 */
bool ShowOSK() {
  HINSTANCE result = ShellExecute(NULL, L"open", L"osk.exe", NULL, NULL, SW_SHOW);
  if ((INT_PTR)result > 32) {
    Print(L"✓ 屏幕键盘已显示");
    return true;
  }
  PrintError(L"✗ 无法显示屏幕键盘");
  return false;
}

/**
 * 显示使用帮助
 */
void ShowHelp() {
  Print(L"Windows 虚拟触摸键盘工具");
  Print(L"");
  Print(L"用法:");
  Print(L"  qiin-tabtip [选项]");
  Print(L"");
  Print(L"选项:");
  Print(L"  show      - 显示触摸键盘 (TabTip)");
  Print(L"  hide      - 隐藏触摸键盘");
  Print(L"  toggle    - 切换键盘显示状态 (默认)");
  Print(L"  osk       - 显示屏幕键盘 (OSK)");
  Print(L"  status    - 检查键盘是否可见");
  Print(L"  diagnose  - 诊断系统环境");
  Print(L"  help      - 显示此帮助信息");
  Print(L"");
  Print(L"示例:");
  Print(L"  qiin-tabtip              # 切换键盘状态");
  Print(L"  qiin-tabtip show         # 显示触摸键盘");
  Print(L"  qiin-tabtip osk          # 显示屏幕键盘");
  Print(L"  qiin-tabtip diagnose     # 诊断问题");
}

int wmain(int argc, wchar_t* argv[]) {
  // 设置控制台 UTF-8 输出
  SetConsoleOutputCP(CP_UTF8);

  const wchar_t* command = L"toggle";
  wchar_t cmdLower[256] = {0};
  
  if (argc > 1) {
    command = argv[1];
    // 转换为小写用于比较
    lstrcpynW(cmdLower, command, 255);
    CharLowerW(cmdLower);
    command = cmdLower;
  }

  if (StrEqualI(command, L"show")) {
    return ShowKeyboard() ? 0 : 1;
  }
  else if (StrEqualI(command, L"hide")) {
    return HideKeyboard() ? 0 : 1;
  }
  else if (StrEqualI(command, L"toggle")) {
    return ToggleKeyboard() ? 0 : 1;
  }
  else if (StrEqualI(command, L"osk")) {
    return ShowOSK() ? 0 : 1;
  }
  else if (StrEqualI(command, L"status")) {
    if (IsKeyboardVisible()) {
      Print(L"触摸键盘当前: 可见");
      return 0;
    } else {
      Print(L"触摸键盘当前: 隐藏");
      return 1;
    }
  }
  else if (StrEqualI(command, L"diagnose") || StrEqualI(command, L"diag")) {
    Diagnose();
    return 0;
  }
  else if (StrEqualI(command, L"help") || StrEqualI(command, L"--help") || 
           StrEqualI(command, L"-h") || StrEqualI(command, L"/?")) {
    ShowHelp();
    return 0;
  }
  else {
    wchar_t errMsg[512];
    wsprintfW(errMsg, L"未知命令: %s", command);
    PrintError(errMsg);
    PrintError(L"使用 'qiin-tabtip help' 查看帮助");
    return 1;
  }

  return 0;
}

// 如果没有 wmain 支持，使用普通的 main 函数
#ifndef _UNICODE
int main(int argc, char* argv[]) {
  // 获取命令行参数的宽字符版本
  LPWSTR* szArglist;
  int nArgs;
  
  szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
  if (szArglist == NULL) {
    PrintError(L"CommandLineToArgvW failed");
    return 1;
  }
  
  int result = wmain(nArgs, szArglist);
  
  LocalFree(szArglist);
  return result;
}
#endif

