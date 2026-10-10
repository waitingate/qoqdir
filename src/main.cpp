#include "app.h"
#include "crashlog.h"
#include "shell.h"
#include "version.h"

#include <shellapi.h>

// ---------------------------------------------------------------- help

namespace {

// F1: the main keys in groups, the most helpful first in each: what Explorer users wouldn't guess comes first. One
// action a line, in plain words (the menus' own where there is one), with its main key: Explorer's other keys for the
// same thing work but aren't listed (Alt+Up, F4, Ctrl+R), nor are Enter and Ctrl+C. The command line last, by
// example, and where to read more. No tab stops in either help: a message box puts them by the font's average letter
// width, so columns line up in one font only.
const wchar_t kKeys[] =
    L"Navigation\n"
    L"  Filter: type in the list, or Ctrl+F\n"
    L"  Clear the filter: Esc\n"
    L"  Up: Backspace\n"
    L"  Bookmark a folder: right-click its tab, or Alt+drag it onto Bookmarks\n"
    L"  Address bar: Ctrl+L\n"
    L"  Back: Alt+Left\n"
    L"  Forward: Alt+Right\n"
    L"  Refresh: F5\n"
    L"\n"
    L"Tabs\n"
    L"  New tab: Ctrl+T\n"
    L"  Close tab: Ctrl+W\n"
    L"  Save tabs: Ctrl+S\n"
    L"  Open saved tabs: Ctrl+O\n"
    L"  Open in new tab: Ctrl+Enter or middle-click\n"
    L"  Switch tab: Ctrl+Tab or Ctrl+1 to 9\n"
    L"  Move tab: Ctrl+Shift+PgUp or Ctrl+Shift+PgDn\n"
    L"\n"
    L"Files\n"
    L"  Show or hide hidden files: Ctrl+H\n"
    L"  New folder: Ctrl+Shift+N\n"
    L"  Rename: F2\n"
    L"  Delete: Delete (to the Recycle Bin)\n"
    L"  Delete permanently: Shift+Delete\n"
    L"  Properties: Alt+Enter\n"
    L"\n"
    L"Command line\n"
    L"  Open folders in tabs: qoqdir -t=\"D:\\logs\" -t=\"D:\\My Files\"\n"
    L"  Or: qoqdir --tab=\"D:\\logs\"\n"
    L"  More detail: type qoqdir --help in a terminal";

// qoqdir --help, -h, -?: printed in a terminal, so the columns line up.
const wchar_t kCommandLine[] =
    L"Qoqdir " QOQDIR_VERSION "\n"
    L"\n"
    L"Usage: qoqdir [options] [folder path ...]\n"
    L"\n"
    L"Opens each folder in a tab and shows the first one.\n"
    L"With no folder, opens your home folder.\n"
    L"\n"
    L"Options:\n"
    L"  -t=\"folder path\", --tab=\"folder path\"   open the folder in a tab\n"
    L"  -h, --help, -?                          show this help\n"
    L"\n"
    L"Examples:\n"
    L"  qoqdir .                               open the current folder\n"
    L"  qoqdir -t=\"D:\\logs\"                    open D:\\logs\n"
    L"  qoqdir -t=\"D:\\logs\" -t=\"D:\\My Files\"   open two tabs\n"
    L"  qoqdir \"D:\\logs\"                       same as -t=\"D:\\logs\"\n"
    L"\n"
    L"Notes:\n"
    L"  Always quote folders. PowerShell needs the quotes after -t=.\n"
    L"  Don't end a quoted folder with a backslash: use \"D:\\My Files\",\n"
    L"  not \"D:\\My Files\\\". For a drive, use \"D:\" (not \"D:\\\").\n"
    L"  A folder that can't be opened yet (like a network drive that\n"
    L"  is not connected) still gets a tab.\n"
    L"\n"
    L"Scripts:\n"
    L"  Each run opens a new window.\n"
    L"  A .bat file waits until the window is closed. PowerShell doesn't.\n"
    L"  Don't wait in a .bat file:  start \"\" qoqdir \"D:\\logs\"\n"
    L"  Wait in PowerShell:         Start-Process qoqdir '\"D:\\logs\"' -Wait\n"
    L"  Exit code: 0 = OK, 1 = error (such as an unknown option).\n"
    L"\n"
    L"Files next to qoqdir.exe:\n"
    L"  qoqdir.ini       window size, sort order, column widths\n"
    L"  Bookmarks\\       shortcuts shown at the top of the tree\n"
    L"  Bookmarks\\Tabs\\  tabs saved with Ctrl+S\n";

void helpBox(HWND owner, const std::wstring& text, UINT icon) {   // titled with the version
    std::wstring title = std::wstring(kAppName) + L" " QOQDIR_VERSION;
    MessageBoxW(owner, text.c_str(), title.c_str(), icon);
}

// Prints wherever the output was sent (a file, a pipe, nul), else to the terminal it was typed in. False with
// neither (a shortcut, Run): then the caller shows a box.
bool printHelp(std::wstring text) {
    for (size_t i = 0; (i = text.find(L'\n', i)) != std::wstring::npos; i += 2) text.insert(i, 1, L'\r');
    DWORD n = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && GetFileType(out) != FILE_TYPE_UNKNOWN) {
        std::string utf8(size_t(WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr)), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), utf8.data(), int(utf8.size()), nullptr, nullptr);
        // PowerShell 7 closes the pipe at once for $h = qoqdir -h: then the terminal.
        if (WriteFile(out, utf8.data(), DWORD(utf8.size()), &n, nullptr)) return true;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return false;
    auto open = [](const wchar_t* name) {
        return CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    };
    HANDLE con = open(L"CONOUT$"), in = open(L"CONIN$");
    // cmd and PowerShell don't wait for a window program, so they may be waiting for the next command already, the
    // cursor past column 0. Print below that line, then send Enter so a fresh input line follows the help. Without
    // it PowerShell 7 draws the next typed line over the help.
    CONSOLE_SCREEN_BUFFER_INFO info{};
    bool midLine = GetConsoleScreenBufferInfo(con, &info) && info.dwCursorPosition.X > 0;
    if (midLine) text = L"\r\n" + text;
    bool ok = WriteConsoleW(con, text.c_str(), DWORD(text.size()), &n, nullptr);
    if (ok && midLine) {
        INPUT_RECORD enter[2] = {};
        for (int i = 0; i < 2; ++i) {   // pressed and let go
            enter[i].EventType = KEY_EVENT;
            enter[i].Event.KeyEvent = {i == 0, 1, VK_RETURN, 0, {L'\r'}, 0};
        }
        WriteConsoleInputW(in, enter, 2, &n);
    }
    CloseHandle(con);
    CloseHandle(in);
    FreeConsole();
    return ok;
}

}  // namespace

void showKeys(HWND owner) { helpBox(owner, kKeys, MB_ICONINFORMATION); }

// ---------------------------------------------------------------- start

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // DLLs that Windows loads by name come from System32 only, never from the exe's folder: a planted
    // propsys.dll next to qoqdir.exe (in Downloads, say) would run inside the app.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    installCrashLog();
    SetErrorMode(SEM_FAILCRITICALERRORS);   // a drive without a disk fails quietly, not with Windows' "No Disk" box
    std::vector<std::wstring> args;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        args.assign(argv + 1, argv + argc);
        LocalFree(argv);
    }
    CommandLine cmd = parseCommandLine(args);   // "qoqdir ." is resolved here, against the terminal's folder
    // Then leave it: a program's working folder cannot be renamed, deleted or ejected. Explorer's is System32.
    wchar_t sys[MAX_PATH];
    if (GetSystemDirectoryW(sys, MAX_PATH)) SetCurrentDirectoryW(sys);
#ifndef QOQDIR_SELFTEST
    if (cmd.selfTest) cmd.error = L"Unknown option: --selftest";   // only qoqdir-test.exe has the self-test
#endif
    if (cmd.help || !cmd.error.empty()) {
        std::wstring text = cmd.error.empty() ? kCommandLine : cmd.error + L"\n\n" + kCommandLine;   // the mistake first
        if (!printHelp(text)) helpBox(nullptr, text, cmd.error.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
        return cmd.error.empty() ? 0 : 1;
    }

    if (FAILED(OleInitialize(nullptr))) return 1;
    int result = 0;
    {
        ShellProcessRef shellRef;
        INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES |
                                                 ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
#ifdef QOQDIR_SELFTEST
        if (cmd.selfTest) cmd.folders = {App::prepareSelfTest()};
#endif

        App app;
        if (app.create(instance, cmd, !cmd.selfTest)) {
#ifdef QOQDIR_SELFTEST
            if (cmd.selfTest) app.startSelfTest(cmd.folders[0], cmd.folders[0] + L".log");
#endif
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (app.handleKey(msg)) continue;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        } else {
            result = 1;
        }
        shellRef.waitForShell();   // let background copies and Properties dialogs finish
        if (cmd.selfTest) result = app.exitCode();
    }
    OleFlushClipboard();   // files copied here stay pasteable after the app has closed (nothing to do otherwise)
    OleUninitialize();
    return result;
}
