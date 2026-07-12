/**
 * @file tools/sunshinesvc.cpp
 * @brief Handles launching Sunshine.exe into user sessions as SYSTEM
 */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <string>
#include <vector>

// PROC_THREAD_ATTRIBUTE_JOB_LIST is currently missing from MinGW headers
#ifndef PROC_THREAD_ATTRIBUTE_JOB_LIST
  #define PROC_THREAD_ATTRIBUTE_JOB_LIST ProcThreadAttributeValue(13, FALSE, TRUE, FALSE)
#endif

SERVICE_STATUS_HANDLE service_status_handle;
SERVICE_STATUS service_status;
HANDLE stop_event;
HANDLE session_change_event;

#define SERVICE_NAME "SunshineService"

DWORD WINAPI
HandlerEx(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext) {
  switch (dwControl) {
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;

    case SERVICE_CONTROL_SESSIONCHANGE:
      // If a new session connects to the console, restart Sunshine
      // to allow it to spawn inside the new console session.
      if (dwEventType == WTS_CONSOLE_CONNECT) {
        SetEvent(session_change_event);
      }
      return NO_ERROR;

    case SERVICE_CONTROL_PRESHUTDOWN:
      // The system is shutting down
    case SERVICE_CONTROL_STOP:
      // Let SCM know we're stopping in up to 30 seconds
      service_status.dwCurrentState = SERVICE_STOP_PENDING;
      service_status.dwControlsAccepted = 0;
      service_status.dwWaitHint = 30 * 1000;
      SetServiceStatus(service_status_handle, &service_status);

      // Trigger ServiceMain() to start cleanup
      SetEvent(stop_event);
      return NO_ERROR;

    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

HANDLE
CreateJobObjectForChildProcess() {
  HANDLE job_handle = CreateJobObjectW(NULL, NULL);
  if (!job_handle) {
    return NULL;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limit_info = {};

  // Kill Sunshine.exe when the final job object handle is closed (which will happen if we terminate unexpectedly).
  // This ensures we don't leave an orphaned Sunshine.exe running with an inherited handle to our log file.
  job_limit_info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

  // Allow Sunshine.exe to use CREATE_BREAKAWAY_FROM_JOB when spawning processes to ensure they can to live beyond
  // the lifetime of SunshineSvc.exe. This avoids unexpected user data loss if we crash or are killed.
  job_limit_info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_BREAKAWAY_OK;

  if (!SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation, &job_limit_info, sizeof(job_limit_info))) {
    CloseHandle(job_handle);
    return NULL;
  }

  return job_handle;
}

LPPROC_THREAD_ATTRIBUTE_LIST
AllocateProcThreadAttributeList(DWORD attribute_count) {
  SIZE_T size;
  InitializeProcThreadAttributeList(NULL, attribute_count, 0, &size);

  auto list = (LPPROC_THREAD_ATTRIBUTE_LIST) HeapAlloc(GetProcessHeap(), 0, size);
  if (list == NULL) {
    return NULL;
  }

  if (!InitializeProcThreadAttributeList(list, attribute_count, 0, &size)) {
    HeapFree(GetProcessHeap(), 0, list);
    return NULL;
  }

  return list;
}

HANDLE
DuplicateTokenForSession(DWORD console_session_id) {
  HANDLE current_token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE, &current_token)) {
    return NULL;
  }

  // Duplicate our own LocalSystem token
  HANDLE new_token;
  if (!DuplicateTokenEx(current_token, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &new_token)) {
    CloseHandle(current_token);
    return NULL;
  }

  CloseHandle(current_token);

  // Change the duplicated token to the console session ID
  if (!SetTokenInformation(new_token, TokenSessionId, &console_session_id, sizeof(console_session_id))) {
    CloseHandle(new_token);
    return NULL;
  }

  return new_token;
}

DWORD
LaunchGuiAgent(DWORD console_session_id) {
  std::wstring service_path(32768, L'\0');
  const auto service_path_length =
    GetModuleFileNameW(NULL, service_path.data(), static_cast<DWORD>(service_path.size()));
  if (service_path_length == 0) {
    const auto error = GetLastError();
    return error == ERROR_SUCCESS ? ERROR_BAD_PATHNAME : error;
  }
  if (service_path_length >= service_path.size()) {
    return ERROR_INSUFFICIENT_BUFFER;
  }
  service_path.resize(service_path_length);

  const auto tools_separator = service_path.find_last_of(L"\\/");
  if (tools_separator == std::wstring::npos || tools_separator == 0) {
    return ERROR_BAD_PATHNAME;
  }
  const auto install_separator = service_path.find_last_of(L"\\/", tools_separator - 1);
  if (install_separator == std::wstring::npos) {
    return ERROR_BAD_PATHNAME;
  }

  const auto install_directory = service_path.substr(0, install_separator);
  const auto gui_directory = install_directory + L"\\assets\\gui";
  const auto gui_path = gui_directory + L"\\sunshine-gui.exe";
  const auto gui_attributes = GetFileAttributesW(gui_path.c_str());
  if (gui_attributes == INVALID_FILE_ATTRIBUTES) {
    return GetLastError();
  }
  if (gui_attributes & FILE_ATTRIBUTE_DIRECTORY) {
    return ERROR_FILE_NOT_FOUND;
  }

  HANDLE user_token = NULL;
  if (!WTSQueryUserToken(console_session_id, &user_token)) {
    return GetLastError();
  }

  LPVOID environment = NULL;
  if (!CreateEnvironmentBlock(&environment, user_token, FALSE)) {
    const auto error = GetLastError();
    CloseHandle(user_token);
    return error;
  }

  auto command = L"\"" + gui_path + L"\" --hidden";
  std::vector<wchar_t> command_line(command.begin(), command.end());
  command_line.push_back(L'\0');

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  startup_info.lpDesktop = (LPWSTR) L"winsta0\\default";

  PROCESS_INFORMATION process_info = {};
  const auto launched = CreateProcessAsUserW(user_token,
    gui_path.c_str(),
    command_line.data(),
    NULL,
    NULL,
    FALSE,
    CREATE_UNICODE_ENVIRONMENT,
    environment,
    gui_directory.c_str(),
    &startup_info,
    &process_info);
  const auto launch_error = launched ? ERROR_SUCCESS : GetLastError();

  if (launched) {
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
  }
  DestroyEnvironmentBlock(environment);
  CloseHandle(user_token);
  return launch_error;
}

void
WriteServiceLog(HANDLE log_file, const std::string &message) {
  DWORD bytes_written;
  const auto line = "[sunshinesvc] " + message + "\r\n";
  WriteFile(log_file, line.data(), static_cast<DWORD>(line.size()), &bytes_written, NULL);
}

bool
ReconcileGuiAgent(HANDLE log_file, DWORD console_session_id, DWORD &last_error) {
  const auto error = LaunchGuiAgent(console_session_id);
  if (error == ERROR_SUCCESS) {
    if (last_error != ERROR_SUCCESS) {
      WriteServiceLog(log_file, "GUI agent launch recovered for session " + std::to_string(console_session_id));
    }
  }
  else if (error != last_error) {
    WriteServiceLog(log_file,
      "GUI agent launch failed for session " + std::to_string(console_session_id) +
        " (Win32 error " + std::to_string(error) + "); retrying");
  }
  last_error = error;
  return error == ERROR_SUCCESS;
}

HANDLE
OpenLogFileHandle() {
  WCHAR log_file_name[MAX_PATH];

  // Create sunshine.log in the Temp folder (usually %SYSTEMROOT%\Temp)
  GetTempPathW(_countof(log_file_name), log_file_name);
  wcscat_s(log_file_name, L"sunshine.log");

  // The file handle must be inheritable for our child process to use it
  SECURITY_ATTRIBUTES security_attributes = { sizeof(security_attributes), NULL, TRUE };

  // Overwrite the old sunshine.log
  return CreateFileW(log_file_name,
    GENERIC_WRITE,
    FILE_SHARE_READ,
    &security_attributes,
    CREATE_ALWAYS,
    0,
    NULL);
}

bool
RunTerminationHelper(HANDLE console_token, DWORD pid) {
  WCHAR module_path[MAX_PATH];
  GetModuleFileNameW(NULL, module_path, _countof(module_path));
  std::wstring command;

  command += L'"';
  command += module_path;
  command += L'"';
  command += L" --terminate " + std::to_wstring(pid);

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  startup_info.lpDesktop = (LPWSTR) L"winsta0\\default";

  // Execute ourselves as a detached process in the user session with the --terminate argument.
  // This will allow us to attach to Sunshine's console and send it a Ctrl-C event.
  PROCESS_INFORMATION process_info;
  if (!CreateProcessAsUserW(console_token,
        module_path,
        (LPWSTR) command.c_str(),
        NULL,
        NULL,
        FALSE,
        CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS,
        NULL,
        NULL,
        &startup_info,
        &process_info)) {
    return false;
  }

  // Wait for the termination helper to complete
  WaitForSingleObject(process_info.hProcess, INFINITE);

  // Check the exit status of the helper process
  DWORD exit_code;
  GetExitCodeProcess(process_info.hProcess, &exit_code);

  // Cleanup handles
  CloseHandle(process_info.hProcess);
  CloseHandle(process_info.hThread);

  // If the helper process returned 0, it succeeded
  return exit_code == 0;
}

VOID WINAPI
ServiceMain(DWORD dwArgc, LPTSTR *lpszArgv) {
  service_status_handle = RegisterServiceCtrlHandlerEx(SERVICE_NAME, HandlerEx, NULL);
  if (service_status_handle == NULL) {
    // Nothing we can really do here but terminate ourselves
    ExitProcess(GetLastError());
    return;
  }

  // Tell SCM we're starting
  service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  service_status.dwServiceSpecificExitCode = 0;
  service_status.dwWin32ExitCode = NO_ERROR;
  service_status.dwWaitHint = 0;
  service_status.dwControlsAccepted = 0;
  service_status.dwCheckPoint = 0;
  service_status.dwCurrentState = SERVICE_START_PENDING;
  SetServiceStatus(service_status_handle, &service_status);

  // Create a manual-reset stop event
  stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
  if (stop_event == NULL) {
    // Tell SCM we failed to start
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  // Create an auto-reset session change event
  session_change_event = CreateEventA(NULL, FALSE, FALSE, NULL);
  if (session_change_event == NULL) {
    // Tell SCM we failed to start
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  auto log_file_handle = OpenLogFileHandle();
  if (log_file_handle == INVALID_HANDLE_VALUE) {
    // Tell SCM we failed to start
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  // We can use a single STARTUPINFOEXW for all the processes that we launch
  STARTUPINFOEXW startup_info = {};
  startup_info.StartupInfo.cb = sizeof(startup_info);
  startup_info.StartupInfo.lpDesktop = (LPWSTR) L"winsta0\\default";
  startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup_info.StartupInfo.hStdInput = NULL;
  startup_info.StartupInfo.hStdOutput = log_file_handle;
  startup_info.StartupInfo.hStdError = log_file_handle;

  // Allocate an attribute list with space for 2 entries
  startup_info.lpAttributeList = AllocateProcThreadAttributeList(2);
  if (startup_info.lpAttributeList == NULL) {
    // Tell SCM we failed to start
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  // Only allow Sunshine.exe to inherit the log file handle, not all inheritable handles
  UpdateProcThreadAttribute(startup_info.lpAttributeList,
    0,
    PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
    &log_file_handle,
    sizeof(log_file_handle),
    NULL,
    NULL);

  // Tell SCM we're running (and stoppable now)
  service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
  service_status.dwCurrentState = SERVICE_RUNNING;
  SetServiceStatus(service_status_handle, &service_status);

  // Loop every 3 seconds until the stop event is set or Sunshine.exe is running
  while (WaitForSingleObject(stop_event, 3000) != WAIT_OBJECT_0) {
    auto console_session_id = WTSGetActiveConsoleSessionId();
    if (console_session_id == 0xFFFFFFFF) {
      // No console session yet
      continue;
    }

    auto console_token = DuplicateTokenForSession(console_session_id);
    if (console_token == NULL) {
      continue;
    }

    // Job objects cannot span sessions, so we must create one for each process
    auto job_handle = CreateJobObjectForChildProcess();
    if (job_handle == NULL) {
      CloseHandle(console_token);
      continue;
    }

    // Start Sunshine.exe inside our job object
    UpdateProcThreadAttribute(startup_info.lpAttributeList,
      0,
      PROC_THREAD_ATTRIBUTE_JOB_LIST,
      &job_handle,
      sizeof(job_handle),
      NULL,
      NULL);

    PROCESS_INFORMATION process_info;
    if (!CreateProcessAsUserW(console_token,
          L"Sunshine.exe",
          NULL,
          NULL,
          NULL,
          TRUE,
          CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
          NULL,
          NULL,
          (LPSTARTUPINFOW) &startup_info,
          &process_info)) {
      CloseHandle(console_token);
      CloseHandle(job_handle);
      continue;
    }

    // Launch the tray in the signed-in user's session so HKCU settings and
    // single-instance ownership remain scoped to that user.
    DWORD gui_agent_error = ERROR_SUCCESS;
    bool gui_agent_started = ReconcileGuiAgent(log_file_handle, console_session_id, gui_agent_error);

    bool still_running;
    do {
      // Wait for the stop event to be set, Sunshine.exe to terminate, or the console session to change
      const HANDLE wait_objects[] = { stop_event, process_info.hProcess, session_change_event };
      const auto wait_result =
        WaitForMultipleObjects(_countof(wait_objects), wait_objects, FALSE, gui_agent_started ? INFINITE : 3000);
      switch (wait_result) {
        case WAIT_TIMEOUT:
          gui_agent_started = ReconcileGuiAgent(log_file_handle, console_session_id, gui_agent_error);
          still_running = true;
          break;

        case WAIT_OBJECT_0 + 2:
          if (WTSGetActiveConsoleSessionId() == console_session_id) {
            // The active console session didn't actually change. Let Sunshine keep running.
            still_running = true;
            continue;
          }
          // Fall-through to terminate Sunshine.exe and start it again.
        case WAIT_OBJECT_0:
          // The service is shutting down, so try to gracefully terminate Sunshine.exe.
          // If it doesn't terminate in 20 seconds, we will forcefully terminate it.
          if (!RunTerminationHelper(console_token, process_info.dwProcessId) ||
              WaitForSingleObject(process_info.hProcess, 20000) != WAIT_OBJECT_0) {
            // If it won't terminate gracefully, kill it now
            TerminateProcess(process_info.hProcess, ERROR_PROCESS_ABORTED);
          }
          still_running = false;
          break;

        case WAIT_OBJECT_0 + 1: {
          // Sunshine terminated itself.

          DWORD exit_code;
          if (GetExitCodeProcess(process_info.hProcess, &exit_code) && exit_code == ERROR_SHUTDOWN_IN_PROGRESS) {
            // Sunshine is asking for us to shut down, so gracefully stop ourselves.
            SetEvent(stop_event);
          }
          still_running = false;
          break;
        }

        default:
          SetEvent(stop_event);
          still_running = false;
          break;
      }
    } while (still_running);

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    CloseHandle(console_token);
    CloseHandle(job_handle);
  }

  // Let SCM know we've stopped
  service_status.dwCurrentState = SERVICE_STOPPED;
  SetServiceStatus(service_status_handle, &service_status);
}

// This will run in a child process in the user session
int
DoGracefulTermination(DWORD pid) {
  // Attach to Sunshine's console
  if (!AttachConsole(pid)) {
    return GetLastError();
  }

  // Disable our own Ctrl-C handling
  SetConsoleCtrlHandler(NULL, TRUE);

  // Send a Ctrl-C event to Sunshine
  if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) {
    return GetLastError();
  }

  return 0;
}

int
main(int argc, char *argv[]) {
  static const SERVICE_TABLE_ENTRY service_table[] = {
    { (LPSTR) SERVICE_NAME, ServiceMain },
    { NULL, NULL }
  };

  // Check if this is a reinvocation of ourselves to send Ctrl-C to Sunshine.exe
  if (argc == 3 && strcmp(argv[1], "--terminate") == 0) {
    return DoGracefulTermination(atol(argv[2]));
  }

  // By default, services have their current directory set to %SYSTEMROOT%\System32.
  // We want to use the directory where Sunshine.exe is located instead of system32.
  // This requires stripping off 2 path components: the file name and the last folder
  WCHAR module_path[MAX_PATH];
  GetModuleFileNameW(NULL, module_path, _countof(module_path));
  for (auto i = 0; i < 2; i++) {
    auto last_sep = wcsrchr(module_path, '\\');
    if (last_sep) {
      *last_sep = 0;
    }
  }
  SetCurrentDirectoryW(module_path);

  // Trigger our ServiceMain()
  return StartServiceCtrlDispatcher(service_table);
}
