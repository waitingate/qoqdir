#include "app.h"
#include "shell.h"

#include <shellapi.h>

namespace {

const wchar_t kUsage[] =
    L"Usage: qoqdir [switches] [folders...]\n"
    L"\n"
    L"  {folder}\tOpen the folder in a tab (relative paths work: qoqdir .)\n"
    L"  -t{folder}\tThe same, as a switch: -tD:\\logs  -t\"C:\\My Files\"\n"
    L"\t(quote folders with spaces, but don't end them with \\ before the closing quote)\n"
    L"  -h, -?\tShow this help\n"
    L"\n"
    L"Tabs are not remembered between runs. Keep folders you use often as\n"
    L"shortcuts in the Bookmarks folder next to qoqdir.exe (shown at the top of the tree).\n"
    L"\n"
    L"Examples:\n"
    L"  qoqdir .\n"
    L"  qoqdir -tD:\\projects -tD:\\logs";

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    std::vector<std::wstring> args;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        args.assign(argv + 1, argv + argc);
        LocalFree(argv);
    }
    CommandLine cmd = parseCommandLine(args);
    if (cmd.help || !cmd.error.empty()) {   // there is no console, so the help is a small window
        std::wstring text = cmd.error.empty() ? kUsage : cmd.error + L"\n\n" + kUsage;
        MessageBoxW(nullptr, text.c_str(), kAppName, cmd.error.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
        return cmd.error.empty() ? 0 : 1;
    }

    if (FAILED(OleInitialize(nullptr))) return 1;
    int result = 0;
    {
        ShellProcessRef shellRef;
        INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES |
                                                 ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        if (cmd.selfTest) cmd.folders = {App::prepareSelfTest()};

        App app;
        if (app.create(instance, cmd, !cmd.selfTest)) {
            if (cmd.selfTest) app.startSelfTest(cmd.folders[0], cmd.folders[0] + L".log");
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
    OleUninitialize();
    return result;
}
