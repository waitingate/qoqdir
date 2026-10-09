#include "app.h"
#include "crashlog.h"
#include "shell.h"
#include "version.h"

#include <shellapi.h>

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
    L"  Or: qoqdir --tab=\"D:\\logs\"\n"
    L"  More detail: type qoqdir --help in a terminal";

// qoqdir --help (or -h, -?): the command line in full, for people who start Qoqdir from scripts and shortcuts.
const wchar_t kCommandLine[] =
    L"qoqdir [switches] [folders...]\n"
    L"\n"
    L"  qoqdir \"D:\\logs\"\n"
    L"  qoqdir -t=\"D:\\logs\" -t=\"D:\\My Files\"\n"
    L"  qoqdir --tab=\"D:\\logs\"\n"
    L"\n"
    L"Each folder opens in a tab. The first one is shown.\n"
    L"The same folder twice gives one tab.\n"
    L"With no folders, the home folder opens.\n"
    L". is the current folder. D: means D:\\.\n"
    L"A folder that can't be opened still gets a tab,\n"
    L"marked \"not available\".\n"
    L"\n"
    L"Quotes\n"
    L"  Keep them: PowerShell needs them after -t=.\n"
    L"  Don't end a quoted folder with \\: write \"D:\\My Files\",\n"
    L"  not \"D:\\My Files\\\". For a drive, write D: alone.\n"
    L"\n"
    L"Windows and scripts\n"
    L"  Each run opens a new window.\n"
    L"  To wait until it closes: start /wait in cmd,\n"
    L"  Start-Process -Wait in PowerShell.\n"
    L"  Exit code: 0, or 1 for an unknown switch.\n"
    L"\n"
    L"Files next to qoqdir.exe\n"
    L"  qoqdir.ini: window size, sort and columns\n"
    L"  Bookmarks: shortcuts shown at the top of the tree\n"
    L"  Bookmarks\\Tabs: the tabs Ctrl+S saved\n"
    L"\n"
    L"Switches\n"
    L"  -t=\"{folder}\", --tab=\"{folder}\": open in a tab\n"
    L"  -h, -?, --help: this help";

void helpBox(HWND owner, const std::wstring& text, UINT icon) {   // titled with the version
    std::wstring title = std::wstring(kAppName) + L" " QOQDIR_VERSION;
    MessageBoxW(owner, text.c_str(), title.c_str(), icon);
}

}  // namespace

void showKeys(HWND owner) { helpBox(owner, kKeys, MB_ICONINFORMATION); }

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
        if (cmd.error.empty()) helpBox(nullptr, kCommandLine, MB_ICONINFORMATION);
        else helpBox(nullptr, cmd.error + L"\n\n" + kCommandLine, MB_ICONWARNING);   // the mistake first
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
