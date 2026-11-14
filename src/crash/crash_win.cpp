/**
 * @file src/crash/crash_win.cpp
 * @brief Windows crash capture: unhandled-exception filter, vectored handler for the unrecoverable codes, terminate / abort / pure-call /
 *        invalid-parameter handlers, MiniDumpWriteDump and the hang report.
 *
 * Sources: SetUnhandledExceptionFilter, AddVectoredExceptionHandler, MiniDumpWriteDump, RtlVirtualUnwind and the CRT handler setters are the
 * Windows APIs this uses (Microsoft Learn, docs entries dr-ms-crash-* in the source index).
 *
 * The thread that crashed does almost nothing: it copies its context and wakes a worker that was created at start with its own stack. The
 * worker writes the dump first (the kernel does the work), then a meta with the faulting frame alone, then walks the stack and rewrites the
 * meta, so a stack too damaged to walk still leaves a report. Nothing on the crash path allocates or takes a lock the crashed thread may own.
 */
#ifdef _WIN32

  #include "crash_state.h"

  #include <csignal>
  #include <cstdlib>
  #include <cstring>
  #include <exception>
  #include <stdexcept>
  #include <string_view>
  #include <thread>

  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  // after windows.h
  #include <dbghelp.h>
  #include <tlhelp32.h>

namespace crash::detail {
  namespace {
    using minidump_write_t = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

    constexpr int max_modules = 384;
    struct mod_t {
      std::uint64_t base = 0, size = 0;
      char name[module_name_size] = {};
    };
    struct snapshot_t {
      mod_t m[max_modules];
      int n = 0;
    };
    snapshot_t g_snap[2];
    std::atomic<int> g_snap_index { 0 };

    wchar_t g_pending_w[path_cap];
    minidump_write_t g_dump_fn = nullptr;
    HANDLE g_req_event = nullptr, g_done_event = nullptr, g_worker = nullptr;
    DWORD g_worker_tid = 0;
    volatile LONG g_crashing = 0;
    int g_tick = 0;

    struct request_t {
      char kind[16];
      char code[48];
      alignas(16) CONTEXT ctx;
      EXCEPTION_RECORD rec;
      EXCEPTION_POINTERS ep;
      DWORD tid;
      int skip;
      bool exception;
    };
    alignas(16) request_t g_req;

    void
    refresh_modules() {
      const int next = 1 - g_snap_index.load();
      auto &snap = g_snap[next];
      snap.n = 0;
      HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
      if (h == INVALID_HANDLE_VALUE) return;
      MODULEENTRY32W e;
      e.dwSize = sizeof(e);
      for (BOOL ok = Module32FirstW(h, &e); ok && snap.n < max_modules; ok = Module32NextW(h, &e)) {
        auto &m = snap.m[snap.n++];
        m.base = (std::uint64_t) (std::uintptr_t) e.modBaseAddr;
        m.size = e.modBaseSize;
        WideCharToMultiByte(CP_UTF8, 0, e.szModule, -1, m.name, (int) sizeof(m.name) - 1, nullptr, nullptr);
        m.name[sizeof(m.name) - 1] = 0;
      }
      CloseHandle(h);
      g_snap_index.store(next);
    }

    bool
    resolve(std::uint64_t pc, frame_t &out, bool allow_system_lookup) {
      const auto &snap = g_snap[g_snap_index.load()];
      for (int i = 0; i < snap.n; ++i) {
        if (pc >= snap.m[i].base && pc < snap.m[i].base + snap.m[i].size) {
          std::memcpy(out.module, snap.m[i].name, sizeof(out.module));
          out.offset = pc - snap.m[i].base;
          return true;
        }
      }
      if (allow_system_lookup) {
        // loaded after the last snapshot; the loader lock may be ours to wait on, so only the second pass asks
        DWORD64 base = 0;
        if (auto *header = RtlPcToFileHeader((PVOID) (std::uintptr_t) pc, (PVOID *) &base); header) {
          wchar_t name[MAX_PATH];
          const DWORD len = GetModuleFileNameW((HMODULE) header, name, MAX_PATH);
          const wchar_t *file = name;
          for (DWORD i = 0; i < len; ++i) {
            if (name[i] == L'\\' || name[i] == L'/') file = name + i + 1;
          }
          WideCharToMultiByte(CP_UTF8, 0, file, -1, out.module, (int) sizeof(out.module) - 1, nullptr, nullptr);
          out.offset = pc - (std::uint64_t) (std::uintptr_t) header;
          return true;
        }
      }
      std::memcpy(out.module, "unknown", 8);
      out.offset = pc;
      return false;
    }

    bool
    readable(std::uint64_t address, std::size_t bytes) {
      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery((LPCVOID) (std::uintptr_t) address, &mbi, sizeof(mbi))) return false;
      if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
      return address + bytes <= (std::uint64_t) (std::uintptr_t) mbi.BaseAddress + mbi.RegionSize;
    }

    /// Frames of a context from the unwind data of the images; stops at a stack it cannot read.
    std::size_t
    unwind(CONTEXT ctx, frame_t *frames, std::size_t cap, bool system_lookup) {
      std::size_t n = 0;
      for (; n < cap; ++n) {
        const std::uint64_t pc = ctx.Rip;
        if (!pc) break;
        resolve(pc, frames[n], system_lookup);
        if (!readable(ctx.Rsp, 16)) {
          ++n;
          break;
        }
        // frames after the first are return addresses: look the function up one byte earlier, a call at the very end of one is common
        const std::uint64_t lookup = n ? pc - 1 : pc;
        DWORD64 image_base = 0;
        auto *entry = RtlLookupFunctionEntry(lookup, &image_base, nullptr);
        if (!entry) {
          ctx.Rip = *(const DWORD64 *) (std::uintptr_t) ctx.Rsp;
          ctx.Rsp += 8;
        }
        else {
          PVOID handler_data = nullptr;
          DWORD64 frame_pointer = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, lookup, entry, &ctx, &handler_data, &frame_pointer, nullptr);
        }
      }
      return n;
    }

    void
    wcat(wchar_t *dst, std::size_t cap, const wchar_t *src) {
      std::size_t n = 0;
      while (dst[n]) ++n;
      for (; *src && n + 1 < cap; ++src) dst[n++] = *src;
      dst[n] = 0;
    }

    void
    wcat_ascii(wchar_t *dst, std::size_t cap, const char *src) {
      std::size_t n = 0;
      while (dst[n]) ++n;
      for (; *src && n + 1 < cap; ++src) dst[n++] = (wchar_t) (unsigned char) *src;
      dst[n] = 0;
    }

    bool
    write_file(const wchar_t *path, const void *data, std::size_t size) {
      HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (f == INVALID_HANDLE_VALUE) return false;
      DWORD written = 0;
      const BOOL ok = WriteFile(f, data, (DWORD) size, &written, nullptr);
      CloseHandle(f);
      return ok && written == size;
    }

    // The smallest dump that still names the faulting place. A minidump is a copy of memory, so what it holds is limited at three levels
    // (MINIDUMP_TYPE, Microsoft Learn): MiniDumpNormal has no heap, no data segments and no handle data; MiniDumpFilterMemory keeps only
    // the pointer-sized values of each stack (a string, a key or a token that lived on a stack is blanked); MiniDumpFilterModulePaths
    // removes the user name from module paths; MiniDumpWithoutOptionalData drops regions that are not essential. MiniDumpWithIndirectlyReferencedMemory
    // (the heap behind each stack pointer) is NOT used: that is where TLS keys, pairing secrets, session tokens and clipboard text live.
    constexpr int dump_type_small = 0x00000000 /* Normal */ | 0x00001000 /* ThreadInfo */ | 0x00000020 /* UnloadedModules */ | 0x00000008 /* FilterMemory */ | 0x00000080 /* FilterModulePaths */ | 0x00000400 /* WithoutOptionalData */ | 0x00004000 /* WithoutAuxiliaryState */;

    // Second level, the callback (MINIDUMP_CALLBACK_OUTPUT): per module only the name and the CodeView record (no data segments, no TLS data, no code), per thread
    // its record, context, stack and thread info (no backing store, no instruction window, no TEB), and no extra memory range at all.
    BOOL CALLBACK
    dump_filter(PVOID, const PMINIDUMP_CALLBACK_INPUT input, PMINIDUMP_CALLBACK_OUTPUT output) {
      if (!input || !output) return TRUE;
      switch ((int) input->CallbackType) {
        case ModuleCallback:
          output->ModuleWriteFlags &= ~(ULONG) (0x0002 /* DataSeg */ | 0x0010 /* ReferencedByMemory */ | 0x0020 /* TlsData */ | 0x0040 /* CodeSegs */);
          return TRUE;
        case ThreadCallback:
        case ThreadExCallback:
          output->ThreadWriteFlags &= ~(ULONG) (0x0008 /* BackingStore */ | 0x0010 /* InstructionWindow */ | 0x0020 /* ThreadData */);
          return TRUE;
        case MemoryCallback:
          return FALSE;  // no additional memory range
        default:
          return TRUE;
      }
    }

    bool
    write_dump(const wchar_t *path, EXCEPTION_POINTERS *ep, DWORD tid) {
      if (!g_dump_fn) return false;
      HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (f == INVALID_HANDLE_VALUE) return false;
      MINIDUMP_EXCEPTION_INFORMATION info { tid, ep, FALSE };
      MINIDUMP_CALLBACK_INFORMATION callback {};
      callback.CallbackRoutine = dump_filter;
      callback.CallbackParam = nullptr;
      bool ok = g_dump_fn(GetCurrentProcess(), GetCurrentProcessId(), f, (MINIDUMP_TYPE) dump_type_small, ep ? &info : nullptr, nullptr, &callback);
      LARGE_INTEGER s {};
      GetFileSizeEx(f, &s);
      if (ok && (std::uint64_t) s.QuadPart > max_dump_bytes) ok = false;
      CloseHandle(f);
      if (!ok) DeleteFileW(path);
      return ok;
    }

    void
    write_meta(const wchar_t *final_path, const char *kind, const char *code, const frame_t *frames, std::size_t count) {
      char buffer[meta_buffer_size];
      meta_t meta;
      meta.product = g.product;
      meta.version = g.version;
      meta.os_version = g.os_version;
      meta.kind = kind;
      meta.fault_code = code;
      meta.time = now_unix();
      meta.uptime_s = (now_ms() - g.start_ms) / 1000;
      meta.frames = frames;
      meta.frame_count = count;
      const auto len = render_meta(meta, buffer, sizeof(buffer));
      if (!len) return;
      wchar_t tmp[path_cap + 160];
      tmp[0] = 0;
      wcat(tmp, _countof(tmp), final_path);
      wcat(tmp, _countof(tmp), L".tmp");
      if (write_file(tmp, buffer, len)) MoveFileExW(tmp, final_path, MOVEFILE_REPLACE_EXISTING);
    }

    /// One report: dump, then the meta with the faulting frame, then the full meta. Runs on the worker (crash) or the watchdog (hang).
    void
    write_reports(const char *kind, const char *code, EXCEPTION_POINTERS *ep, DWORD tid, const CONTEXT &ctx, int skip) {
      if (!g.enabled.load()) return;
      char stem[112];
      stem_noalloc(stem, sizeof(stem), now_unix(), kind, GetCurrentProcessId());
      wchar_t base[path_cap + 160];
      base[0] = 0;
      wcat(base, _countof(base), g_pending_w);
      wcat(base, _countof(base), L"\\");
      wcat_ascii(base, _countof(base), stem);
      wchar_t dump_path[path_cap + 160], meta_path[path_cap + 160];
      dump_path[0] = meta_path[0] = 0;
      wcat(dump_path, _countof(dump_path), base);
      wcat(dump_path, _countof(dump_path), L".dmp");
      wcat(meta_path, _countof(meta_path), base);
      wcat(meta_path, _countof(meta_path), L".json");

      write_dump(dump_path, ep, tid);

      frame_t first;
      resolve(ctx.Rip, first, false);
      write_meta(meta_path, kind, code, &first, 1);

      frame_t frames[max_frames + 4];
      auto count = unwind(ctx, frames, max_frames + 4, true);
      std::size_t from = (std::size_t) skip < count ? (std::size_t) skip : 0;
      if (count - from > max_frames) count = from + max_frames;
      write_meta(meta_path, kind, code, frames + from, count - from);
    }

    DWORD WINAPI
    worker_main(LPVOID) {
      for (;;) {
        WaitForSingleObject(g_req_event, INFINITE);
        write_reports(g_req.kind, g_req.code, g_req.exception ? &g_req.ep : nullptr, g_req.tid, g_req.ctx, g_req.skip);
        SetEvent(g_done_event);
      }
    }

    void
    set_text(char *dst, std::size_t cap, const char *src) {
      std::size_t n = 0;
      for (; src[n] && n + 1 < cap; ++n) dst[n] = src[n];
      dst[n] = 0;
    }

    [[noreturn]] void
    finish(UINT exit_code) {
      TerminateProcess(GetCurrentProcess(), exit_code);
      for (;;) Sleep(INFINITE);
    }

    /// The crashing thread: hand over to the worker, wait for it (bounded), end the process with the code the default handling would give.
    [[noreturn]] __attribute__((noinline)) void
    report_and_die(const char *kind, const char *code, EXCEPTION_POINTERS *ep, UINT exit_code) {
      if (GetCurrentThreadId() == g_worker_tid) finish(exit_code);  // the reporter itself crashed
      if (InterlockedCompareExchange(&g_crashing, 1, 0) != 0) {
        // another thread is reporting; this one waits for the process to end
        for (;;) Sleep(INFINITE);
      }
      if (g.enabled.load() && g_worker) {
        set_text(g_req.kind, sizeof(g_req.kind), kind);
        set_text(g_req.code, sizeof(g_req.code), code);
        g_req.tid = GetCurrentThreadId();
        if (ep) {
          g_req.rec = *ep->ExceptionRecord;
          g_req.ctx = *ep->ContextRecord;
          g_req.ep.ExceptionRecord = &g_req.rec;
          g_req.ep.ContextRecord = &g_req.ctx;
          g_req.exception = true;
          g_req.skip = 0;
        }
        else {
          // no exception behind it (abort, terminate): a context here, the two frames of this function and its handler left out
          std::memset(&g_req.rec, 0, sizeof(g_req.rec));
          RtlCaptureContext(&g_req.ctx);
          g_req.rec.ExceptionCode = 0xE0000001;
          g_req.rec.ExceptionAddress = (PVOID) (std::uintptr_t) g_req.ctx.Rip;
          g_req.ep.ExceptionRecord = &g_req.rec;
          g_req.ep.ContextRecord = &g_req.ctx;
          g_req.exception = true;
          g_req.skip = 2;
        }
        SetEvent(g_req_event);
        WaitForSingleObject(g_done_event, 60 * 1000);
      }
      finish(exit_code);
    }

    void
    hex_code(char *out, DWORD code) {
      out[0] = '0';
      out[1] = 'x';
      for (int i = 0; i < 8; ++i) out[2 + i] = "0123456789ABCDEF"[(code >> (28 - 4 * i)) & 15];
      out[10] = 0;
    }

    LONG WINAPI
    unhandled_filter(EXCEPTION_POINTERS *ep) {
      const DWORD c = ep->ExceptionRecord->ExceptionCode;
      // a C++ exception nobody caught (GCC's SEH code, MSVC's): it reaches this filter before std::terminate on a thread's way out; the stack
      // here is the throw site's, which is better than terminate's
      if (c == 0x20474343 || c == 0xE06D7363) report_and_die("abort", "uncaught_exception", ep, c);
      char code[16];
      hex_code(code, c);
      report_and_die("crash", code, ep, c);
    }

    /// Only the codes no program recovers from: a first-chance access violation is often caught on purpose (CPU probes, guarded reads).
    LONG CALLBACK
    vectored_fatal(EXCEPTION_POINTERS *ep) {
      const DWORD c = ep->ExceptionRecord->ExceptionCode;
      if (c == 0xC00000FD /* stack overflow */ || c == 0xC0000374 /* heap corruption */) {
        char code[16];
        hex_code(code, c);
        report_and_die("crash", code, ep, c);
      }
      return EXCEPTION_CONTINUE_SEARCH;
    }

    [[noreturn]] __attribute__((noinline)) void
    on_terminate() {
      report_and_die("abort", "terminate", nullptr, 3);
    }

    __attribute__((noinline)) void
    on_sigabrt(int) {
      report_and_die("abort", "SIGABRT", nullptr, 3);
    }

    __attribute__((noinline)) void
    on_purecall() {
      report_and_die("abort", "purecall", nullptr, 3);
    }

    __attribute__((noinline)) void
    on_invalid_parameter(const wchar_t *, const wchar_t *, const wchar_t *, unsigned int, std::uintptr_t) {
      report_and_die("abort", "invalid_parameter", nullptr, 3);
    }
  }  // namespace

  void
  platform_install() {
    MultiByteToWideChar(CP_UTF8, 0, g.pending_utf8, -1, g_pending_w, (int) _countof(g_pending_w));
    refresh_modules();
    // dbghelp from System32 only; resolved now because the loader may be locked when it is needed
    wchar_t path[MAX_PATH];
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n && n < MAX_PATH - 16) {
      wcat(path, MAX_PATH, L"\\dbghelp.dll");
      if (HMODULE m = LoadLibraryW(path)) g_dump_fn = (minidump_write_t) (void *) GetProcAddress(m, "MiniDumpWriteDump");
    }
    g_req_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_done_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_worker = CreateThread(nullptr, 512 * 1024, worker_main, nullptr, 0, &g_worker_tid);
    SetUnhandledExceptionFilter(unhandled_filter);
    AddVectoredExceptionHandler(1, vectored_fatal);
    std::set_terminate(on_terminate);
    std::signal(SIGABRT, on_sigabrt);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_purecall_handler(on_purecall);
    _set_invalid_parameter_handler(on_invalid_parameter);
  }

  void
  platform_tick() {
    // a library may replace the filter; ours goes back
    SetUnhandledExceptionFilter(unhandled_filter);
    if (++g_tick % 30 == 0) refresh_modules();
  }

  void
  platform_bind_thread(slot_t &slot) {
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    slot.thread_token = (std::uint64_t) (std::uintptr_t) h;
  }

  void
  platform_release_thread(slot_t &slot) {
    if (slot.thread_token) CloseHandle((HANDLE) (std::uintptr_t) slot.thread_token);
    slot.thread_token = 0;
  }

  void
  platform_report_hang(int index) {
    auto &slot = g.slots[index];
    HANDLE thread = (HANDLE) (std::uintptr_t) slot.thread_token;
    alignas(16) CONTEXT ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    bool suspended = false, have_context = false;
    if (thread && SuspendThread(thread) != (DWORD) -1) {
      suspended = true;
      have_context = GetThreadContext(thread, &ctx) != 0;
    }
    char code[48] = "hang:";
    std::size_t n = 5;
    for (const char *s = slot.name; *s && n + 1 < sizeof(code); ++s) code[n++] = *s;
    code[n] = 0;

    char stem[112];
    stem_noalloc(stem, sizeof(stem), now_unix(), "hang", GetCurrentProcessId());
    wchar_t base[path_cap + 160];
    base[0] = 0;
    wcat(base, _countof(base), g_pending_w);
    wcat(base, _countof(base), L"\\");
    wcat_ascii(base, _countof(base), stem);
    wchar_t dump_path[path_cap + 160], meta_path[path_cap + 160];
    dump_path[0] = meta_path[0] = 0;
    wcat(dump_path, _countof(dump_path), base);
    wcat(dump_path, _countof(dump_path), L".dmp");
    wcat(meta_path, _countof(meta_path), base);
    wcat(meta_path, _countof(meta_path), L".json");

    frame_t frames[max_frames + 4];
    std::size_t count = 0;
    if (have_context) count = unwind(ctx, frames, max_frames, true);
    if (!count) {
      // no stack to read: the slot's name stands in, so a hang in the same place still dedupes
      std::strncpy(frames[0].module, "hang", sizeof(frames[0].module) - 1);
      std::strncat(frames[0].module, "-", sizeof(frames[0].module) - strlen(frames[0].module) - 1);
      std::strncat(frames[0].module, slot.name, sizeof(frames[0].module) - strlen(frames[0].module) - 1);
      frames[0].offset = 0;
      count = 1;
    }
    write_dump(dump_path, nullptr, 0);  // the dump first: the meta is what the uploader looks for
    write_meta(meta_path, "hang", code, frames, count);
    if (suspended) ResumeThread(thread);
  }

  void
  platform_test_crash(const char *what) {
    const std::string_view mode { what };
    if (mode == "null-deref") {
      volatile int *p = nullptr;
      *p = 42;
    }
    else if (mode == "abort") {
      std::abort();
    }
    else if (mode == "terminate") {
      std::terminate();
    }
    else if (mode == "throw") {
      // an exception nobody catches, on a thread: the way a real one reaches std::terminate
      std::thread([] { throw std::runtime_error("SUNSHINE_TEST_CRASH throw"); }).join();
    }
    else if (mode == "hang") {
      const int slot = watch_register("test-hang", 3000);
      watch_busy(slot);
      Sleep(9000);  // past the timeout and one watchdog tick; the report is written while this sleeps
      watch_idle(slot);
      watch_release(slot);
      TerminateProcess(GetCurrentProcess(), 0);
    }
  }
}  // namespace crash::detail

#endif  // _WIN32
