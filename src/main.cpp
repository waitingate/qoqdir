#include "app.h"
#include "crashlog.h"
#include "shell.h"
#include "version.h"

#include <shellapi.h>

namespace {

// The main keys in groups, the most helpful first in each: what Explorer users wouldn't guess comes first. One action
// a line, in plain words (the menus' own where there is one), with its main key: Explorer's other keys for the same
// thing work but aren't listed (Alt+Up, F4, Ctrl+R), nor are Enter and Ctrl+C. The command line last, by example.
// No tab stops: a message box puts them by the font's average letter width, so columns line up in one font only.
const wchar_t kUsage[] =
    L"Navigation\n"
    L"  Filter: type in the list, or Ctrl+F\n"
    L"  Clear the filter: Esc\n"
    L"  Up: Backspace\n"
    L"  Bookmark a folder: Alt+drag it onto Bookmarks in the tree\n"
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
    L"  This help: qoqdir -h, or F1 in the window";

}  // namespace

// The help box, titled with the version: qoqdir -h (the mistake first, if there was one) and F1 in the window.
void showHelp(HWND owner, const std::wstring& error) {
    std::wstring text = error.empty() ? kUsage : error + L"\n\n" + kUsage;
    std::wstring title = std::wstring(kAppName) + L" " QOQDIR_VERSION;
    MessageBoxW(owner, text.c_str(), title.c_str(), error.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
}

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
    if (cmd.selfTest) cmd.error = L"Unknown switch: --selftest";   // only qoqdir-test.exe has the self-test
#endif
    if (cmd.help || !cmd.error.empty()) {   // there is no console, so the help is a small window
        showHelp(nullptr, cmd.error);
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
