/**
 * @file tools/test_args.cpp
 * @brief 测试程序：输出所有命令行参数到日志文件
 * @note 此程序仅用于开发测试，不会被打包到发布版本中
 */

#include <iostream>
#include <fstream>
#include <string>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <sddl.h>
#include <userenv.h>
#include <lmcons.h>  // For UNLEN, MAX_COMPUTERNAME_LENGTH
#include <cstring>
#ifndef _MSC_VER
// MinGW doesn't support #pragma comment, link libraries in CMakeLists.txt instead
#else
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "userenv.lib")
#endif
#else
#include <unistd.h>
#include <pwd.h>
#include <limits.h>
#include <cstring>
#endif

std::string get_current_time() {
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

#ifdef _WIN32
void print_user_info(std::ofstream& log) {
    log << "----------------------------------------\n";
    log << "User Information (用户信息):\n";
    
    // 获取用户名
    char username[UNLEN + 1];
    DWORD username_len = UNLEN + 1;
    if (GetUserNameA(username, &username_len)) {
        log << "  Username (用户名): " << username << "\n";
    } else {
        log << "  Username (用户名): <Failed to get (获取失败)>\n";
    }
    
    // 获取计算机名
    char computer_name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD computer_name_len = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameA(computer_name, &computer_name_len)) {
        log << "  Computer (计算机名): " << computer_name << "\n";
    }
    
    // 检查是否是管理员
    BOOL is_admin = FALSE;
    PSID admin_group = NULL;
    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt_authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admin_group)) {
        CheckTokenMembership(NULL, admin_group, &is_admin);
        FreeSid(admin_group);
    }
    log << "  Is Admin (是否管理员): " << (is_admin ? "Yes (是)" : "No (否)") << "\n";
    
    // 获取当前进程的令牌信息
    HANDLE token = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        // 获取用户 SID
        DWORD token_user_size = 0;
        GetTokenInformation(token, TokenUser, NULL, 0, &token_user_size);
        if (token_user_size > 0) {
            std::vector<BYTE> token_user_buf(token_user_size);
            PTOKEN_USER token_user = reinterpret_cast<PTOKEN_USER>(token_user_buf.data());
            if (GetTokenInformation(token, TokenUser, token_user, token_user_size, &token_user_size)) {
                LPSTR sid_string = NULL;
                if (ConvertSidToStringSidA(token_user->User.Sid, &sid_string)) {
                    log << "  User SID (用户 SID): " << sid_string << "\n";
                    LocalFree(sid_string);
                }
            }
        }
        
        // 获取权限级别
        DWORD elevation_type_size = sizeof(TOKEN_ELEVATION_TYPE);
        TOKEN_ELEVATION_TYPE elevation_type;
        if (GetTokenInformation(token, TokenElevationType, &elevation_type,
                                elevation_type_size, &elevation_type_size)) {
            const char* elevation_str = "Unknown (未知)";
            switch (elevation_type) {
                case TokenElevationTypeDefault:
                    elevation_str = "Default (默认)";
                    break;
                case TokenElevationTypeFull:
                    elevation_str = "Full (Elevated) (完全/已提升)";
                    break;
                case TokenElevationTypeLimited:
                    elevation_str = "Limited (受限)";
                    break;
            }
            log << "  Elevation Type (权限提升类型): " << elevation_str << "\n";
        }
        
        // 检查是否以管理员身份运行
        BOOL is_elevated = FALSE;
        DWORD is_elevated_size = sizeof(BOOL);
        if (GetTokenInformation(token, TokenElevation, &is_elevated,
                               is_elevated_size, &is_elevated_size)) {
            log << "  Is Elevated (是否已提升权限): " << (is_elevated ? "Yes (是)" : "No (否)") << "\n";
        }
        
        CloseHandle(token);
    }
    
    // 获取进程 ID
    log << "  Process ID (进程 ID): " << GetCurrentProcessId() << "\n";
    log << "  Thread ID (线程 ID): " << GetCurrentThreadId() << "\n";
    
    // 获取会话 ID
    DWORD session_id = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
        log << "  Session ID (会话 ID): " << session_id << "\n";
    }
}
#else
void print_user_info(std::ofstream& log) {
    log << "----------------------------------------\n";
    log << "User Information (用户信息):\n";
    
    // 获取用户 ID 和组 ID
    uid_t uid = getuid();
    gid_t gid = getgid();
    log << "  UID (用户 ID): " << uid << "\n";
    log << "  GID (组 ID): " << gid << "\n";
    
    // 获取用户名
    struct passwd* pw = getpwuid(uid);
    if (pw) {
        log << "  Username (用户名): " << pw->pw_name << "\n";
        log << "  Home (主目录): " << pw->pw_dir << "\n";
    }
    
    // 检查是否是 root
    log << "  Is Root (是否 Root): " << (uid == 0 ? "Yes (是)" : "No (否)") << "\n";
    
    // 获取进程 ID
    log << "  Process ID (进程 ID): " << getpid() << "\n";
}
#endif

int main(int argc, char* argv[]) {
    // 获取可执行文件所在目录
    std::string log_file;
#ifdef _WIN32
    char exe_path[MAX_PATH];
    DWORD path_len = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    if (path_len > 0 && path_len < MAX_PATH) {
        // 找到最后一个反斜杠
        char* last_slash = strrchr(exe_path, '\\');
        if (last_slash) {
            *last_slash = '\0';  // 截断到目录
            log_file = std::string(exe_path) + "\\sunshine_test_args.log";
        } else {
            log_file = "sunshine_test_args.log";  // 回退到当前目录
        }
    } else {
        log_file = "sunshine_test_args.log";  // 回退到当前目录
    }
#else
    char exe_path[PATH_MAX];
    ssize_t path_len = readlink("/proc/self/exe", exe_path, PATH_MAX - 1);
    if (path_len > 0) {
        exe_path[path_len] = '\0';
        char* last_slash = strrchr(exe_path, '/');
        if (last_slash) {
            *last_slash = '\0';  // 截断到目录
            log_file = std::string(exe_path) + "/sunshine_test_args.log";
        } else {
            log_file = "sunshine_test_args.log";  // 回退到当前目录
        }
    } else {
        log_file = "sunshine_test_args.log";  // 回退到当前目录
    }
#endif

    std::ofstream log(log_file, std::ios::app);
    if (!log.is_open()) {
        std::cerr << "Failed to open log file: " << log_file << std::endl;
        return 1;
    }

    // 写入分隔符和时间戳
    log << "\n";
    log << "========================================\n";
    log << "Test Time (测试时间): " << get_current_time() << "\n";
    log << "========================================\n";
    log << "Total Arguments (参数总数): " << argc << "\n";
    log << "Executable (可执行文件): " << (argc > 0 ? argv[0] : "unknown") << "\n";
    
    // 打印用户权限信息
    print_user_info(log);
    
    log << "----------------------------------------\n";

    // 输出所有参数
    for (int i = 0; i < argc; i++) {
        log << "Arg[" << i << "] (参数[" << i << "]): \"" << argv[i] << "\"\n";
    }

    log << "----------------------------------------\n";
    log << "Argument Analysis (参数分析):\n";

    // 检查是否有环境变量相关的参数
    bool found_env_vars = false;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.find("%SUNSHINE_") != std::string::npos) {
            log << "  WARNING (警告): Found unexpanded environment variable in arg[" << i << "] (在参数[" << i << "] 中发现未展开的环境变量): " << arg << "\n";
            found_env_vars = true;
        }
    }

    if (!found_env_vars) {
        log << "  ✓ All environment variables appear to be expanded (所有环境变量已正确展开)\n";
    }

    log << "========================================\n";
    log << "\n";

    log.close();

    // 同时输出到控制台（如果可用）
    std::cout << "Arguments logged to (参数已记录到): " << log_file << std::endl;
    std::cout << "Total arguments (参数总数): " << argc << std::endl;
    for (int i = 0; i < argc; i++) {
        std::cout << "  [" << i << "] " << argv[i] << std::endl;
    }

    return 0;
}

