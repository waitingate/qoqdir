#include "crashlog.h"

#include "folder.h"

#include <string>
#include "version.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cxxabi.h>
#include <exception>
#include <typeinfo>

// Spec 4.10: an internal bug stops the program and %TEMP%\qoqdir-crash.log says why. Hooks: CPU faults
// (onCrash), failed _GLIBCXX_ASSERTIONS checks (at the bottom), an exception nothing caught (onTerminate) and
// abort() (onAbort). Lines are built in stack buffers and written with Win32 calls: the heap may be what broke
// (naming an exception's type is the one heap use, and only then). Addresses are exe offsets (ASLR);
// to name one, use the debug copy release.ps1 keeps: addr2line -e qoqdir-<version>-debug.exe 0x140000000+offset

namespace {

wchar_t logPath[MAX_PATH];
const char* exeBase = nullptr;
SIZE_T exeSize = 0;
bool logged = false;

// "exe+0x1234" for an address in the exe; the bare address for one in a system DLL or a shell extension.
const char* describe(const void* p, char* out, size_t size) {
    auto a = static_cast<const char*>(p);
    if (a >= exeBase && a < exeBase + exeSize)
        snprintf(out, size, "exe+0x%llx", static_cast<unsigned long long>(a - exeBase));
    else
        snprintf(out, size, "0x%llx (outside the exe)", reinterpret_cast<unsigned long long>(p));
    return out;
}

void writeLine(const char* what) {
    HANDLE f = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1200];
    int n = snprintf(line, sizeof line, "%04u-%02u-%02u %02u:%02u:%02u  v" QOQDIR_VERSION "  %s\r\n", t.wYear,
                     t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, what);
    DWORD written = 0;
    if (n > 0) WriteFile(f, line, DWORD(n < int(sizeof line) ? n : int(sizeof line) - 1), &written, nullptr);
    CloseHandle(f);
}

void logCrash(const char* what) {
    if (logged) return;   // a failed assertion is logged with its text; the abort() it leads to adds nothing
    logged = true;
    writeLine(what);
}

LONG WINAPI onCrash(EXCEPTION_POINTERS* e) {
    char at[64], what[128];
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    snprintf(what, sizeof what, "Exception 0x%08lX  [%s]", static_cast<unsigned long>(r->ExceptionCode),
             describe(r->ExceptionAddress, at, sizeof at));
    logCrash(what);
    return EXCEPTION_EXECUTE_HANDLER;   // ends the process without Windows' own error window
}

void onAbort(int) {
    logCrash("abort() was called");
    // Exit 3, as msvcrt's abort() does; the UCRT's would __fastfail with a false "stack buffer overrun" report.
    TerminateProcess(GetCurrentProcess(), 3);
}

// An exception nothing caught: its type and message, then abort() as the library would.
void onTerminate() {
    char what[1024];
    try {
        if (std::exception_ptr e = std::current_exception()) std::rethrow_exception(e);
        logCrash("terminate() was called with no exception");
    } catch (const std::exception& e) {
        int status = 0;
        char* name = abi::__cxa_demangle(typeid(e).name(), nullptr, nullptr, &status);
        snprintf(what, sizeof what, "Uncaught %s: %s", status == 0 && name ? name : typeid(e).name(), e.what());
        free(name);
        logCrash(what);
    } catch (...) {
        logCrash("Uncaught exception of a type that is not a std::exception");
    }
    abort();
}

// ---------------------------------------------------------------- hangs

HWND g_watched = nullptr;
DWORD g_windowThread = 0;

// One frame as "exe+0x1234" or "name.dll+0x1234" (the bare address when it is in no module).
const char* frameText(DWORD64 pc, char* out, size_t size) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(pc), &m) || !m) {
        snprintf(out, size, "0x%llx", static_cast<unsigned long long>(pc));
        return out;
    }
    auto offset = static_cast<unsigned long long>(pc - reinterpret_cast<DWORD64>(m));
    if (reinterpret_cast<const char*>(m) == exeBase) {
        snprintf(out, size, "exe+0x%llx", offset);
        return out;
    }
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(m, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    snprintf(out, size, "%ls+0x%llx", name ? name + 1 : path, offset);
    return out;
}

// The unwind entry for pc, from its module's own exception table. RtlLookupFunctionEntry would do this, but it takes
// the loader's table lock: a thread stopped while the loader holds it (a DLL being mapped) would never be resumed.
RUNTIME_FUNCTION* functionEntry(DWORD64 pc, DWORD64* base) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(pc), &mbi, sizeof mbi) || mbi.Type != MEM_IMAGE) return nullptr;
    auto image = static_cast<char*>(mbi.AllocationBase);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto table = reinterpret_cast<RUNTIME_FUNCTION*>(image + dir.VirtualAddress);
    DWORD rva = DWORD(pc - reinterpret_cast<DWORD64>(image));
    for (size_t lo = 0, hi = dir.Size / sizeof(RUNTIME_FUNCTION); lo < hi;) {
        size_t mid = (lo + hi) / 2;
        if (rva < table[mid].BeginAddress) hi = mid;
        else if (rva >= table[mid].EndAddress) lo = mid + 1;
        else {
            *base = reinterpret_cast<DWORD64>(image);
            RUNTIME_FUNCTION* f = &table[mid];
            return f->UnwindData & 1 ? reinterpret_cast<RUNTIME_FUNCTION*>(image + (f->UnwindData & ~1u)) : f;   // indirect
        }
    }
    return nullptr;
}

// Every second: once the window is hung, its thread's stack, once per hang. While the thread is stopped nothing that
// takes a lock runs: its registers are read and the frames walked from the modules' own unwind tables (a thread's
// stack in this process is readable; a function without unwind data is a leaf, whose return address is on top of the
// stack). The frames are named once it runs again: naming a module takes a loader lock, and a thread hung inside a
// DLL load (a shell extension) holds that lock, so the watcher would wait for the very thread it has stopped, for good.
DWORD WINAPI hangWatch(void*) {
    bool said = false;
    for (;; Sleep(1000)) {
        if (!IsWindow(g_watched)) return 0;
        if (!IsHungAppWindow(g_watched)) {
            said = false;
            continue;
        }
        if (said) continue;
        said = true;
        DWORD64 frames[24];
        int count = 0;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, g_windowThread);
        if (t && SuspendThread(t) != DWORD(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(t, &c)) {
                for (; count < 24 && c.Rip; ++count) {
                    frames[count] = c.Rip;
                    DWORD64 base = 0;
                    RUNTIME_FUNCTION* fn = functionEntry(c.Rip, &base);
                    if (!fn) {
                        c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
                        c.Rsp += 8;
                        continue;
                    }
                    void* handler = nullptr;
                    DWORD64 frameBase = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &handler, &frameBase, nullptr);
                }
            }
            ResumeThread(t);
        }
        if (t) CloseHandle(t);
        char line[1200];
        int n = snprintf(line, sizeof line, "Not responding for 5 s; the window thread is at");
        for (int i = 0; i < count && n < int(sizeof line) - 100; ++i) {
            char frame[300];
            n += snprintf(line + n, sizeof line - size_t(n), "%s %s", i ? " <" : "", frameText(frames[i], frame, sizeof frame));
        }
        writeLine(line);
    }
}

}  // namespace

void crashLogIn(const std::wstring& dir) {
    lstrcpynW(logPath, joinPath(dir, toLower(kAppName) + L"-crash.log").c_str(), MAX_PATH);
}

void watchForHangs(HWND window) {
    g_watched = window;
    g_windowThread = GetCurrentThreadId();
    if (HANDLE t = CreateThread(nullptr, 0, hangWatch, nullptr, 0, nullptr)) CloseHandle(t);
}

void installCrashLog() {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    lstrcpynW(logPath, joinPath(temp, toLower(kAppName) + L"-crash.log").c_str(), MAX_PATH);
    exeBase = reinterpret_cast<const char*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exeBase);
    exeSize = reinterpret_cast<const IMAGE_NT_HEADERS*>(exeBase + dos->e_lfanew)->OptionalHeader.SizeOfImage;
    ULONG guard = 64 * 1024;   // after a stack overflow the filter runs on what is left of the stack: keep room
    SetThreadStackGuarantee(&guard);
    SetUnhandledExceptionFilter(onCrash);
    std::set_terminate(onTerminate);
    signal(SIGABRT, onAbort);
    _set_error_mode(_OUT_TO_STDERR);   // or msvcrt's abort() first shows a "Microsoft Visual C++ Runtime Library" box
}

// libstdc++ calls this when a _GLIBCXX_ASSERTIONS check fails. Its own writes to a stderr a windowed program
// lacks; this one logs the check, then stops. (Defined here, it replaces the library's in the static link.)
void std::__glibcxx_assert_fail(const char* file, int line, const char* function, const char* condition) noexcept {
    char at[64], what[1024];
    snprintf(what, sizeof what, "%s:%d: %s: Assertion '%s' failed  [%s]", file, line, function, condition,
             describe(__builtin_return_address(0), at, sizeof at));
    logCrash(what);
    abort();
}
