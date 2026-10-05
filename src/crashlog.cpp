#include "crashlog.h"

#include "folder.h"
#include "version.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>

// Spec 4.10: an internal bug stops the program and %TEMP%\qoqdir-crash.log says why. Hooks: CPU faults
// (onCrash), failed _GLIBCXX_ASSERTIONS checks (at the bottom) and abort() (onAbort). Lines are built in
// stack buffers and written with Win32 calls: the heap may be what broke. Addresses are exe offsets (ASLR);
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

void logCrash(const char* what) {
    if (logged) return;   // a failed assertion is logged with its text; the abort() it leads to adds nothing
    logged = true;
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

}  // namespace

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
