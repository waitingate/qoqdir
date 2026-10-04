#include "crashlog.h"

#include "folder.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>

// Spec 4.10: an internal bug stops the program, and %TEMP%\qoqdir-crash.log says why. A bug ends
// the program in one of three ways, and each has its hook here:
//   - a CPU fault (a bad pointer, a stack overflow...): the unhandled-exception filter, onCrash;
//   - a failed _GLIBCXX_ASSERTIONS check (a bad index...): libstdc++ calls std::__glibcxx_assert_fail,
//     defined at the bottom so the check's text reaches the log instead of a stderr nobody sees;
//   - abort() for anything else (std::terminate...): the SIGABRT handler, onAbort.
// The log line is built in stack buffers with snprintf and written with Win32 calls, nothing from the
// heap, since a corrupted heap may be what broke. Code addresses are logged as offsets into the exe
// because its load address moves (ASLR). To name the line: build the same sources with -g added and
// -s removed, then  addr2line -e qoqdir.exe 0x140000000+offset  (0x140000000 is the exe's image base).

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
    int n = snprintf(line, sizeof line, "%04u-%02u-%02u %02u:%02u:%02u  %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour,
                     t.wMinute, t.wSecond, what);
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
    // msvcrt's abort() would now _exit(3); the UCRT's would go on to a __fastfail (exit 0xC0000409 and a
    // Windows error report about a "stack buffer overrun" that never happened). End it the same way on both.
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

// libstdc++ calls this when a _GLIBCXX_ASSERTIONS check fails. Its own version prints to stderr, which
// a windowed program does not have, so this one puts the same text in the crash log, then stops the
// program as the library would. (Defining it here keeps the library's copy out of the static link.)
void std::__glibcxx_assert_fail(const char* file, int line, const char* function, const char* condition) noexcept {
    char at[64], what[1024];
    snprintf(what, sizeof what, "%s:%d: %s: Assertion '%s' failed  [%s]", file, line, function, condition,
             describe(__builtin_return_address(0), at, sizeof at));
    logCrash(what);
    abort();
}
