// Test aid, Windows: PARTYBOARD_CATCH_FASTFAIL=1 runs the game as a child
// under a minimal debugger (this process) to catch the one death nothing
// inside the game can report: __fastfail (exit code 0xC0000409), which a /GS
// stack-cookie failure, a Control Flow Guard failure or a C runtime in its
// default state ends in. The game's own crash reporter never runs for it, and
// Windows Error Reporting keeps local dumps only through HKLM (machine-wide)
// settings, which this project does not change.
//
// The debugger passes every exception on to the game untouched (its own
// handlers and crash reporter work as before) and only acts on a fail-fast:
// it writes a minidump and the faulting thread's stack (module+offset and
// function names) next to the game's crash files, and logs "[catcher] ..."
// lines to the game's output. Off unless the variable is set; a debugged
// game is slower to start and is not how players run it.

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr const char *kCatcherChild = "PARTYBOARD_CATCHER_CHILD";

void say(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    const int len = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (len <= 0) return;
    std::fputs(line, stderr);
    std::fflush(stderr);
}

const char *fastFailName(ULONG_PTR code)
{
    switch (code) {
    case 2: return "FAST_FAIL_STACK_COOKIE_CHECK_FAILURE (/GS: a stack buffer was overrun)";
    case 3: return "FAST_FAIL_CORRUPT_LIST_ENTRY";
    case 5: return "FAST_FAIL_INVALID_ARG (C runtime invalid parameter)";
    case 7: return "FAST_FAIL_FATAL_APP_EXIT (abort() with error reporting, or a Rust abort)";
    case 10: return "FAST_FAIL_GUARD_ICALL_CHECK_FAILURE (Control Flow Guard: bad indirect call)";
    case 13: return "FAST_FAIL_INVALID_BUFFER_ACCESS";
    case 14: return "FAST_FAIL_INVALID_BALANCED_TREE";
    case 23: return "FAST_FAIL_RANGE_CHECK_FAILURE";
    case 38: return "FAST_FAIL_INVALID_FILE_OPERATION";
    default: return "other fail-fast code";
    }
}

using MiniDumpWriteDumpFn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

std::string outputPath(const char *stem, DWORD pid, const char *extension)
{
    const char *dir = std::getenv("PARTYBOARD_CRASH_DIR");
    std::string path = dir && *dir ? std::string(dir) + "\\" : std::string();
    return path + stem + "-pid-" + std::to_string(pid) + "." + extension;
}

void describeAddress(HANDLE process, DWORD64 address, char *out, size_t size)
{
    char moduleName[MAX_PATH] = "?";
    DWORD64 base = SymGetModuleBase64(process, address);
    if (base) {
        IMAGEHLP_MODULE64 info {};
        info.SizeOfStruct = sizeof(info);
        if (SymGetModuleInfo64(process, base, &info)) {
            std::snprintf(moduleName, sizeof(moduleName), "%s", info.ModuleName);
        }
    }
    alignas(SYMBOL_INFO) unsigned char storage[sizeof(SYMBOL_INFO) + 256] = {};
    auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    if (SymFromAddr(process, address, &displacement, symbol)) {
        std::snprintf(out, size, "%s+0x%llx %s+0x%llx", moduleName,
            static_cast<unsigned long long>(base ? address - base : address), symbol->Name,
            static_cast<unsigned long long>(displacement));
    } else {
        std::snprintf(out, size, "%s+0x%llx", moduleName,
            static_cast<unsigned long long>(base ? address - base : address));
    }
}

// The fail-fast: dump the child and walk the faulting thread's stack.
void reportFastFail(HANDLE process, DWORD pid, DWORD tid, const EXCEPTION_RECORD &record)
{
    const ULONG_PTR failCode = record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0;
    say("[catcher] the game fail-fast-exited: 0x%08lX, code %llu = %s, at 0x%llx\n",
        static_cast<unsigned long>(record.ExceptionCode), static_cast<unsigned long long>(failCode),
        fastFailName(failCode), static_cast<unsigned long long>(reinterpret_cast<DWORD64>(record.ExceptionAddress)));

    HANDLE thread = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
    CONTEXT context {};
    context.ContextFlags = CONTEXT_FULL;
    const bool haveContext = thread && GetThreadContext(thread, &context);

    // Minidump of the child, with the exception as the child saw it.
    if (HMODULE dbghelp = LoadLibraryA("dbghelp.dll")) {
        auto writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
        const std::string dumpPath = outputPath("fastfail", pid, "dmp");
        HANDLE file = CreateFileA(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (writeDump && file != INVALID_HANDLE_VALUE) {
            EXCEPTION_RECORD recordCopy = record;
            EXCEPTION_POINTERS pointers { &recordCopy, &context };
            MINIDUMP_EXCEPTION_INFORMATION info {};
            info.ThreadId = tid;
            info.ExceptionPointers = &pointers;
            info.ClientPointers = FALSE;
            const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo
                | MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory);
            const BOOL ok = writeDump(process, pid, file, type, haveContext ? &info : nullptr, nullptr, nullptr);
            CloseHandle(file);
            if (ok) {
                say("[catcher] minidump: %s\n", dumpPath.c_str());
            } else {
                DeleteFileA(dumpPath.c_str());
                say("[catcher] minidump could not be written (error %lu)\n", GetLastError());
            }
        } else if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
    }

    // The faulting thread's stack, named, into the log and a text file.
    if (!haveContext) {
        say("[catcher] no thread context: stack not available\n");
        if (thread) CloseHandle(thread);
        return;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);
    STACKFRAME64 frame {};
#if defined(_M_AMD64)
    const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_ARM64)
    const DWORD machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = context.Pc;
    frame.AddrFrame.Offset = context.Fp;
    frame.AddrStack.Offset = context.Sp;
#endif
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    const std::string textPath = outputPath("fastfail", pid, "txt");
    FILE *text = std::fopen(textPath.c_str(), "w");
    if (text) {
        std::fprintf(text, "fail-fast 0x%08lX code %llu = %s\nthread %lu\n",
            static_cast<unsigned long>(record.ExceptionCode), static_cast<unsigned long long>(failCode),
            fastFailName(failCode), static_cast<unsigned long>(tid));
    }
    for (int depth = 0; depth < 48; ++depth) {
        if (!StackWalk64(machine, process, thread, &frame, &context, nullptr, SymFunctionTableAccess64,
                SymGetModuleBase64, nullptr)
            || frame.AddrPC.Offset == 0) {
            break;
        }
        char where[512];
        describeAddress(process, frame.AddrPC.Offset, where, sizeof(where));
        say("[catcher] frame %2d %s\n", depth, where);
        if (text) std::fprintf(text, "frame %2d 0x%llx %s\n", depth,
            static_cast<unsigned long long>(frame.AddrPC.Offset), where);
    }
    if (text) {
        std::fclose(text);
        say("[catcher] stack written to %s\n", textPath.c_str());
    }
    SymCleanup(process);
    CloseHandle(thread);
}

} // namespace

extern "C" bool PartyBoard_WinFastFailCatcher(int *exitCode)
{
    const char *enabled = std::getenv("PARTYBOARD_CATCH_FASTFAIL");
    if (!enabled || enabled[0] == '\0' || enabled[0] == '0' || std::getenv(kCatcherChild) != nullptr) {
        return false;
    }
    SetEnvironmentVariableA(kCatcherChild, "1");
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = GetCommandLineW();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, DEBUG_ONLY_THIS_PROCESS, nullptr, nullptr,
            &si, &pi)) {
        say("[catcher] could not start the game under the fail-fast catcher (error %lu); running it directly\n",
            GetLastError());
        SetEnvironmentVariableA(kCatcherChild, nullptr);
        return false;
    }
    say("[catcher] PARTYBOARD_CATCH_FASTFAIL: game process %lu runs under the fail-fast catcher\n",
        static_cast<unsigned long>(pi.dwProcessId));
    // The debugger thread must not be paused by the game's debug events for
    // longer than it takes to pass them on; it does nothing else.
    DWORD code = 0;
    bool reported = false;
    for (;;) {
        DEBUG_EVENT event {};
        if (!WaitForDebugEvent(&event, INFINITE)) break;
        DWORD status = DBG_CONTINUE;
        switch (event.dwDebugEventCode) {
        case EXCEPTION_DEBUG_EVENT: {
            const EXCEPTION_RECORD &record = event.u.Exception.ExceptionRecord;
            if (record.ExceptionCode == EXCEPTION_BREAKPOINT || record.ExceptionCode == 0x4000001F /* WOW64 bp */) {
                status = DBG_CONTINUE; // the loader's initial breakpoint
                break;
            }
            if (record.ExceptionCode == STATUS_STACK_BUFFER_OVERRUN && !reported) {
                reported = true;
                reportFastFail(pi.hProcess, pi.dwProcessId, event.dwThreadId, record);
            }
            // Everything goes back to the game as if no debugger were here.
            status = DBG_EXCEPTION_NOT_HANDLED;
            break;
        }
        case CREATE_PROCESS_DEBUG_EVENT:
            if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            code = event.u.ExitProcess.dwExitCode;
            break;
        default:
            break;
        }
        const bool exited = event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT;
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status);
        if (exited) break;
    }
    WaitForSingleObject(pi.hProcess, 5000);
    if (code == 0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    say("[catcher] game exited with code %lu (0x%08lX)%s\n", static_cast<unsigned long>(code),
        static_cast<unsigned long>(code), reported ? " after a fail-fast (see above)" : "");
    *exitCode = static_cast<int>(code);
    return true;
}
#endif
