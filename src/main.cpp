#include "app.h"
#include "shell.h"

#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    if (FAILED(OleInitialize(nullptr))) return 1;
    int result = 0;
    {
        ShellProcessRef shellRef;
        INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES |
                                                 ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);

        std::wstring start;   // optional folder given on the command line
        bool selfTest = false;
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
            if (argc > 1) selfTest = _wcsicmp(argv[1], L"--selftest") == 0;
            if (argc > 1 && !selfTest) start = resolveArgument(argv[1]);
            LocalFree(argv);
        }
        if (selfTest) start = App::prepareSelfTest();

        App app;
        if (app.create(instance, start, !selfTest)) {
            if (selfTest) app.startSelfTest(start, start + L".log");
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
        if (selfTest) result = app.exitCode();
    }
    OleUninitialize();
    return result;
}
