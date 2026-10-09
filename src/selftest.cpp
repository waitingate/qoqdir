// "qoqdir-test.exe --selftest": a scripted session in the real app. Keys go straight to App::onKey, never to
// other windows. Only qoqdir-test.exe links this file in; users get qoqdir.exe without it.

#include "app.h"

#include <dbt.h>
#include <shellapi.h>

#include <functional>
#include <thread>

struct App::SelfTest {
    struct Step {
        std::wstring name;
        std::function<void()> act;
        std::function<bool()> done;
        ULONGLONG timeoutMs = 5000;
        bool whileLoading = false;   // start without waiting for the current folder load to finish
        bool copies = false;         // puts files on the clipboard: done again while they aren't there (selfTestTick)
    };
    std::vector<Step> steps;
    size_t index = 0;
    bool started = false, busy = false;
    ULONGLONG startTick = 0;
    std::wstring dir, logPath, note;
    int failures = 0;
};

namespace {

std::wstring testRoot() {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    GetLongPathNameW(temp, temp, MAX_PATH);   // paths Windows hands back (shortcut targets) are long: never RUNNER~1
    return joinPath(temp, toLower(kAppName) + L"-selftest");
}

void writeFile(const std::wstring& p) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, "x", 1, &written, nullptr);
    CloseHandle(f);
}

void deleteTree(const std::wstring& dir) {
    Listing l = listFolder(dir, true);
    for (const Item& it : l.items) {
        std::wstring p = joinPath(dir, it.name);
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (it.attrs & FILE_ATTRIBUTE_REPARSE_POINT) {   // a junction or link: remove it, never what it points to
            if (it.isDir()) RemoveDirectoryW(p.c_str());
            else DeleteFileW(p.c_str());
        } else if (it.isDir()) {
            deleteTree(p);
        } else {
            DeleteFileW(p.c_str());
        }
    }
    RemoveDirectoryW(dir.c_str());
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::vector<std::wstring> clipboardFiles() {
    std::vector<std::wstring> files;
    if (!OpenClipboard(nullptr)) return files;
    if (HANDLE data = GetClipboardData(CF_HDROP)) {
        // The names one after another, read straight: DragQueryFileW(i) starts from the first name each time, which
        // took 9 s for 10,000 (and the window showed "not responding").
        auto* drop = static_cast<const DROPFILES*>(GlobalLock(data));
        if (drop && drop->fWide)
            for (auto* p = reinterpret_cast<const wchar_t*>(reinterpret_cast<const char*>(drop) + drop->pFiles); *p;
                 p += wcslen(p) + 1)
                files.emplace_back(p);
        GlobalUnlock(data);
    }
    CloseClipboard();
    return files;
}

void makeShortcut(const std::wstring& lnk, const std::wstring& target) {   // target "": This PC, which has no path
    Com<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void**>(link.put()))))
        return;
    PIDLIST_ABSOLUTE pc = nullptr;
    if (!target.empty()) link->SetPath(target.c_str());
    else if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &pc))) link->SetIDList(pc);
    CoTaskMemFree(pc);
    if (Com<IPersistFile> file = link.as<IPersistFile>()) file->Save(lnk.c_str(), TRUE);
}

double nowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * 1000.0 / double(f.QuadPart);
}

}  // namespace

std::wstring App::prepareSelfTest() {
    std::wstring root = testRoot();
    deleteTree(root);
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(joinPath(root, L"sub").c_str(), nullptr);
    for (const wchar_t* name : {L"file1.txt", L"file2.txt", L"file10.txt", L"\u5831\u544A.pdf"})
        writeFile(joinPath(root, name));
    return root;
}

void App::selfTestKey(WPARAM vk, bool ctrl, bool shift, bool alt) {
    if (onKey({alt ? UINT(WM_SYSKEYDOWN) : UINT(WM_KEYDOWN), vk, GetFocus(), ctrl, shift, alt})) return;
    HWND focus = GetFocus();   // not ours: let the control handle it, as a real key press would
    SendMessageW(focus, WM_KEYDOWN, vk, 0);
    // A real Enter, Esc or Backspace also sends a character (Ctrl+Backspace: 127), if the control is still there.
    if (GetFocus() == focus && (vk == VK_RETURN || vk == VK_ESCAPE || vk == VK_BACK))
        SendMessageW(focus, WM_CHAR, vk == VK_BACK && ctrl ? 127 : vk, 0);
}

void App::selfTestType(const std::wstring& text) {
    for (wchar_t c : text)
        if (!onKey({WM_CHAR, WPARAM(c), GetFocus(), false, false, false})) SendMessageW(GetFocus(), WM_CHAR, c, 0);
}

void App::startSelfTest(const std::wstring& dir, const std::wstring& logPath) {
    selfTest_ = std::make_shared<SelfTest>();
    SelfTest& t = *selfTest_;
    t.dir = dir;
    t.logPath = logPath;
    if (HANDLE f = CreateFileW(logPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        f != INVALID_HANDLE_VALUE)
        CloseHandle(f);   // the log starts empty; every step then appends its line (selfTestLog)
    auto path = [dir](const std::wstring& name) { return joinPath(dir, name); };
    auto idle = [this] { return pending_.generation == 0; };
    auto at = [this, idle](const std::wstring& p) { return idle() && samePath(tab().path, p); };
    auto statusMessage = [this] {
        std::wstring s(size_t(LOWORD(SendMessageW(status_, SB_GETTEXTLENGTHW, 1, 0))) + 1, L'\0');
        SendMessageW(status_, SB_GETTEXTW, 1, reinterpret_cast<LPARAM>(s.data()));
        return std::wstring(s.c_str());
    };
    auto listed = [this](const std::wstring& name) {
        for (int i = 0; i < list_.count(); ++i)
            if (list_.at(i).name == name) return true;
        return false;
    };
    auto editText = [this] {
        HWND edit = ListView_GetEditControl(list_.hwnd());
        wchar_t buf[MAX_PATH] = {};
        if (edit) GetWindowTextW(edit, buf, MAX_PATH);
        return std::wstring(buf);
    };
    auto tabPoint = [this](int index) {   // the middle of a tab, in screen coordinates, where a drag is aimed
        RECT r{};
        TabCtrl_GetItemRect(tabBar_, index, &r);
        POINT pt{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        ClientToScreen(tabBar_, &pt);
        return POINTL{pt.x, pt.y};
    };
    auto caption = [this](int index) {   // a tab's name as the tab bar shows it
        wchar_t text[128] = {};
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = text;
        item.cchTextMax = 128;
        TabCtrl_GetItem(tabBar_, index, &item);
        return std::wstring(text);
    };
    auto tabWidth = [this] {   // the width every tab has (0: they differ)
        int width = 0;
        for (int i = 0; i < TabCtrl_GetItemCount(tabBar_); ++i) {
            RECT r{};
            TabCtrl_GetItemRect(tabBar_, i, &r);
            if (i > 0 && r.right - r.left != width) return 0;
            width = r.right - r.left;
        }
        return width;
    };
    auto goTo = [this](const std::wstring& text) {   // types into the address bar and presses Enter
        SetWindowTextW(address_, text.c_str());
        SetFocus(address_);
        selfTestKey(VK_RETURN);
    };
    auto add = [&t](std::wstring name, std::function<void()> act, std::function<bool()> done, ULONGLONG timeout = 5000,
                    bool whileLoading = false) -> SelfTest::Step& {
        t.steps.push_back({std::move(name), std::move(act), std::move(done), timeout, whileLoading});
        return t.steps.back();
    };
    auto backHome = [&](const wchar_t* name) {   // a step that opens the test folder again
        add(name, [=, this] { open(dir, Nav::New); }, [=, this] { return at(dir); });
    };
    static double t0 = 0, keyMs = 0;   // keyMs: how long a key press kept the window busy
    static int rows = 0;               // list rows before a refresh that must wait
    static DWORD dragEffect = 0;       // what a simulated drag was allowed to do
    static std::wstring next;          // the item after the one Delete removes
    static int top = 0;                // a scroll position to come back to

    // 6 items: the 5 test files and folders, plus the self-test's own Bookmarks folder.
    add(L"opens the start folder with all 6 items", nullptr, [=, this] { return at(dir) && list_.count() == 6; });
    add(L"shows a Chinese file name", nullptr, [=, this] { return listed(L"\u5831\u544A.pdf"); });
    add(L"the folder Qoqdir was started from is not kept open (its working folder is the system folder)", nullptr,
        [=, this] {
            wchar_t cwd[MAX_PATH] = {}, sys[MAX_PATH] = {};
            GetCurrentDirectoryW(MAX_PATH, cwd);
            GetSystemDirectoryW(sys, MAX_PATH);
            selfTest_->note = L"working folder " + std::wstring(cwd);
            return samePath(cwd, sys);
        });
    add(L"a drive without a disk fails quietly, without Windows' \"No Disk\" box", nullptr,
        [] { return (GetErrorMode() & SEM_FAILCRITICALERRORS) != 0; });
    add(L"typing in the list filters it", [=, this] { SetFocus(list_.hwnd()); selfTestType(L"file"); },
        [=, this] { return list_.count() == 3 && GetFocus() == filter_; });
    add(L"Esc clears the filter and returns to the list", [=, this] { selfTestKey(VK_ESCAPE); },
        [=, this] { return list_.count() == 6 && GetFocus() == list_.hwnd(); });
    // An input method hands over a whole word at once: its letters wait in the queue for the list together.
    add(L"a word an input method types at once goes into the filter box whole, not just its first letter",
        [=, this] {
            SetFocus(list_.hwnd());
            for (wchar_t c : std::wstring(L"\u5831\u544A")) PostMessageW(list_.hwnd(), WM_CHAR, c, 1);
        },
        [=, this] {
            selfTest_->note = L"filter \"" + windowText(filter_) + L"\"";
            return windowText(filter_) == L"\u5831\u544A" && list_.count() == 1;
        });
    add(L"(Esc clears it)", [=, this] { selfTestKey(VK_ESCAPE); },
        [=, this] { return list_.count() == 6 && GetFocus() == list_.hwnd(); });
    // AltGr arrives as Ctrl + Alt: the key must type its character, not fire the Ctrl shortcut.
    add(L"AltGr+L types into the filter instead of focusing the address bar",
        [=, this] {
            selfTestKey('L', true, false, true);                                  // the key down
            onKey({WM_CHAR, WPARAM(L'\u0142'), GetFocus(), true, false, true});   // then the character it makes (l-stroke)
        },
        [=, this] { return GetFocus() == filter_ && GetWindowTextLengthW(filter_) == 1 && list_.count() == 0; });
    add(L"Ctrl+Backspace in the filter box deletes the last word, as in Explorer (not a box character)",
        [=, this] {
            SetWindowTextW(filter_, L"");
            SetFocus(list_.hwnd());
            selfTestType(L"file 1");
            selfTestKey(VK_BACK, true);
        },
        [=, this] {
            selfTest_->note = L"filter \"" + windowText(filter_) + L"\"";
            return windowText(filter_) == L"file ";
        });
    add(L"Ctrl+Backspace in the address bar deletes the last word too",
        [=, this] {
            SetFocus(address_);
            SetWindowTextW(address_, L"D:\\my files");
            SendMessageW(address_, EM_SETSEL, 11, 11);   // the caret at the end
            selfTestKey(VK_BACK, true);
        },
        [=, this] {
            selfTest_->note = L"address \"" + windowText(address_) + L"\"";
            return windowText(address_) == L"D:\\my ";
        });
    add(L"(Esc puts the path back and returns to the list)", [=, this] { selfTestKey(VK_ESCAPE); },
        [=, this] { return windowText(address_) == locationText(tab().path) && GetFocus() == list_.hwnd(); });
    // The tree's width (96-DPI units) before a drag, and what a drag far past the window's edge left it at.
    auto drag = std::make_shared<std::pair<int, int>>();
    add(L"dragging the splitter past the window's edge keeps the width shown (what is saved), not one past the edge",
        [=, this] {
            RECT rc{};
            GetClientRect(wnd_, &rc);
            int y = rc.bottom / 2, before = treeWidth_;
            SendMessageW(wnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(splitterX_ + 1, y));
            SendMessageW(wnd_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(rc.right + 1000, y));
            *drag = {before, treeWidth_};
            SendMessageW(wnd_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(scale(before, dpi_), y));   // and back
            SendMessageW(wnd_, WM_LBUTTONUP, 0, MAKELPARAM(scale(before, dpi_), y));
        },
        [=, this] {
            RECT rc{};
            GetClientRect(wnd_, &rc);
            int limit = MulDiv(std::max(scale(80, dpi_), int(rc.right) - scale(200, dpi_)), 96, int(dpi_));
            selfTest_->note = L"far " + std::to_wstring(drag->second) + L", limit " + std::to_wstring(limit) +
                              L", back to " + std::to_wstring(treeWidth_);
            return drag->second <= limit + 1 && std::abs(treeWidth_ - drag->first) <= 1 && !dragging_;
        });
    // Paste only the test's own file: after a failed Ctrl+C, pasting would move files the user cut.
    auto holdsTestFile = [=] {
        auto files = clipboardFiles();
        return files.size() == 1 && samePath(files[0], path(L"file1.txt"));
    };
    auto testFileOnClipboard = [=, this] {   // put there again if another program has taken the clipboard since
        Item file;
        file.name = L"file1.txt";
        file.attrs = FILE_ATTRIBUTE_ARCHIVE;
        if (!holdsTestFile()) copyToClipboard(wnd_, dir, {file}, false);
        return holdsTestFile();
    };
    add(L"Ctrl+C puts the file on the clipboard",
        [=, this] {
            SetWindowTextW(filter_, L"");
            SetFocus(list_.hwnd());
            list_.select({L"file1.txt"}, L"file1.txt");
            selfTestKey('C', true);
        },
        holdsTestFile).copies = true;
    add(L"filter + Enter opens a folder", [=, this] { selfTestType(L"sub"); selfTestKey(VK_RETURN); },
        [=, this] { return at(path(L"sub")); });
    add(L"Ctrl+V pastes with the Windows copy engine", [=, this] { if (testFileOnClipboard()) selfTestKey('V', true); },
        [=, this] { return exists(path(L"sub\\file1.txt")); }, 10000);
    add(L"pasted file appears in the list by itself", nullptr, [=, this] { return idle() && listed(L"file1.txt"); });
    add(L"Paste shortcut makes a shortcut to the copied file",
        [=, this] { if (testFileOnClipboard()) pasteInto(wnd_, list_.dir(), true); },
        [=, this] {
            bool folder = false;
            for (const Item& it : listFolder(path(L"sub"), false).items)
                if (isShortcut(it.name) &&
                    samePath(shortcutTarget(path(L"sub\\" + it.name), &folder).value_or(L""), path(L"file1.txt")))
                    return true;
            return false;
        }, 10000);
    add(L"Backspace goes up and selects the folder you came from", [=, this] { selfTestKey(VK_BACK); },
        [=, this] { return at(dir) && list_.focusedName() == L"sub" && list_.selectedNames().size() == 1; });
    add(L"Alt+Left goes back", [=, this] { selfTestKey(VK_LEFT, false, false, true); }, [=, this] { return at(path(L"sub")); });
    add(L"Alt+Right goes forward", [=, this] { selfTestKey(VK_RIGHT, false, false, true); }, [=, this] { return at(dir); });
    add(L"Ctrl+Shift+N creates a folder and starts renaming it", [=, this] { selfTestKey('N', true, true); },
        [=, this] { return idle() && list_.renaming() && isDirectory(path(L"New folder")); });
    add(L"Enter in the rename box renames the new folder",
        [=, this] {
            SetWindowTextW(ListView_GetEditControl(list_.hwnd()), L"made");
            selfTestKey(VK_RETURN);
        },
        [=, this] { return idle() && isDirectory(path(L"made")) && !exists(path(L"New folder")) && list_.focusedName() == L"made"; });
    add(L"Ctrl+Shift+N while a refresh is pending still selects and renames the new folder afterwards",
        [=, this] {
            SetFocus(list_.hwnd());
            refresh();   // a load is in flight, as after F5 or a change notification on a slow folder
            selfTestKey('N', true, true);
        },
        [=, this] {
            // "New folder (2)" if the folder of the earlier step is still there (its rename could not run)
            return idle() && list_.renaming() && list_.focusedName().starts_with(L"New folder");
        });
    add(L"(cancel that rename and remove the folder)",
        [=, this] {
            ListView_CancelEditLabel(list_.hwnd());
            RemoveDirectoryW(path(L"New folder").c_str());
            RemoveDirectoryW(path(L"New folder (2)").c_str());
            refresh();
        },
        [=, this] { return idle() && !listed(L"New folder") && !listed(L"New folder (2)"); });
    add(L"F2 selects the name without its extension",
        [=, this] { SetFocus(list_.hwnd()); selfTestType(L"file2"); selfTestKey(VK_DOWN); selfTestKey(VK_F2); },
        [=, this] {
            DWORD from = 0, to = 0;
            HWND edit = ListView_GetEditControl(list_.hwnd());
            if (edit) SendMessageW(edit, EM_GETSEL, WPARAM(&from), LPARAM(&to));
            return edit && editText() == L"file2.txt" && from == 0 && to == 5;
        });
    add(L"typing a new name keeps the extension",
        [=, this] {
            SendMessageW(ListView_GetEditControl(list_.hwnd()), EM_REPLACESEL, TRUE, LPARAM(L"renamed"));
            selfTestKey(VK_RETURN);
        },
        [=, this] { return idle() && exists(path(L"renamed.txt")) && !exists(path(L"file2.txt")); });
    add(L"a refresh that lands during a rename waits for it",
        [=, this] {
            SetWindowTextW(filter_, L"");
            writeFile(path(L"added.txt"));   // sorts first, so every other file moves down one row
            rows = list_.count();
            refresh();                       // lands while the rename box below is open
            SetFocus(list_.hwnd());
            list_.select({L"\u5831\u544A.pdf"}, L"\u5831\u544A.pdf");   // untouched by the steps above
            selfTestKey(VK_F2);
            SetWindowTextW(ListView_GetEditControl(list_.hwnd()), L"renamed2.pdf");
        },
        [=, this] { return idle() && list_.renaming() && editText() == L"renamed2.pdf" && list_.count() == rows; });
    add(L"Enter then renames the right file, and the refresh runs",
        [=, this] { selfTestKey(VK_RETURN); },
        [=, this] {
            return idle() && !list_.renaming() && exists(path(L"renamed2.pdf")) && !exists(path(L"\u5831\u544A.pdf")) &&
                   listed(L"added.txt");
        });
    add(L"Delete moves the file to the Recycle Bin and selects the item after it",
        [=, this] {
            SetWindowTextW(filter_, L"");   // unfiltered, so there is an item after it (4.6)
            SetFocus(list_.hwnd());
            int row = list_.select({L"file10.txt"}, L"file10.txt");
            next = row >= 0 && row + 1 < list_.count() ? list_.at(row + 1).name : L"";
            selfTestKey(VK_DELETE);
            // Listed until the reload lands, but it must not stay selected (a second Delete would fail).
            if (!list_.selection().empty()) selfTest_->note = L"the deleted file stayed selected until the reload";
        },
        [=, this] {
            return idle() && !exists(path(L"file10.txt")) && list_.selectedNames() == std::vector<std::wstring>{next} &&
                   selfTest_->note.empty();
        },
        10000);
    add(L"files created by other programs appear by themselves, and the selection stays (4.5)",
        [=, this] {
            SetWindowTextW(filter_, L"");
            list_.select({L"file1.txt"}, L"file1.txt");
            writeFile(path(L"external.txt"));
        },
        [=, this] { return idle() && listed(L"external.txt") && list_.focusedName() == L"file1.txt"; });
    add(L"opening a folder", [=, this] { CreateDirectoryW(path(L"gone").c_str(), nullptr); open(path(L"gone"), Nav::New); },
        [=, this] { return at(path(L"gone")); });
    add(L"...that another program deletes stays in its tab, shown as not available",
        [=, this] { RemoveDirectoryW(path(L"gone").c_str()); },
        [=, this] {
            return at(path(L"gone")) && list_.count() == 0 && statusMessage().find(L"not available") != std::wstring::npos;
        });
    // The tab keeps its gone folder (4.3), which a command must not get as its working folder.
    add(L"(F5 while it is still gone keeps it shown as not available)", [=, this] { selfTestKey(VK_F5); },
        [=, this] {
            return at(path(L"gone")) && list_.count() == 0 && statusMessage().find(L"not available") != std::wstring::npos;
        });
    add(L"a command typed there still runs (the dead folder is not its working folder)",
        [=, this] { goTo(L"rundll32"); },
        [=, this] { return at(path(L"gone")) && statusMessage().find(L"not available") != std::wstring::npos; });
    add(L"F5 tries again once it is back", [=, this] { CreateDirectoryW(path(L"gone").c_str(), nullptr); selfTestKey(VK_F5); },
        [=, this] { return at(path(L"gone")) && statusMessage().empty(); });
    add(L"(back up to the test folder)", [=, this] { SetFocus(list_.hwnd()); selfTestKey(VK_BACK); }, [=, this] { return at(dir); });
    add(L"Ctrl+T opens a tab of the same folder", [=, this] { selfTestKey('T', true); },
        [=, this] { return idle() && tabs_.size() == 2 && at(dir); });
    add(L"Ctrl+W closes it", [=, this] { selfTestKey('W', true); }, [=, this] { return idle() && tabs_.size() == 1 && at(dir); });
    add(L"Ctrl+W on the last tab keeps it (the last tab cannot be closed)", [=, this] { selfTestKey('W', true); },
        [=, this] { return idle() && tabs_.size() == 1 && at(dir); });
    add(L"Ctrl+Shift+PgDn moves the tab one place right, and no further; it stays the current tab",
        [=, this] {
            newTab(path(L"sub"), false);   // a second tab, right of this one
            selfTestKey(VK_NEXT, true, true);
            selfTestKey(VK_NEXT, true, true);
        },
        [=, this] {
            return tabs_.size() == 2 && samePath(tabs_[0].path, path(L"sub")) && active_ == 1 &&
                   TabCtrl_GetCurSel(tabBar_) == 1 && at(dir);
        });
    add(L"Ctrl+Shift+PgUp moves it back, and no further",
        [=, this] {
            selfTestKey(VK_PRIOR, true, true);
            selfTestKey(VK_PRIOR, true, true);
        },
        [=, this] {
            return tabs_.size() == 2 && samePath(tabs_[1].path, path(L"sub")) && active_ == 0 &&
                   TabCtrl_GetCurSel(tabBar_) == 0 && at(dir);
        });
    add(L"(close that tab)", [=, this] { closeTab(1); }, [=, this] { return idle() && tabs_.size() == 1 && at(dir); });
    auto dragAlongBar = [this](int from, int x) {   // a mouse drag of tab 'from' to x, as the tab bar gets it
        RECT r{};
        TabCtrl_GetItemRect(tabBar_, from, &r);
        int y = (r.top + r.bottom) / 2;
        SendMessageW(tabBar_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM((r.left + r.right) / 2, y));
        SendMessageW(tabBar_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y));
        SendMessageW(tabBar_, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    };
    auto tabEdge = [this](int index, bool right) {   // a tab's left or right edge, just inside it
        RECT r{};
        TabCtrl_GetItemRect(tabBar_, index, &r);
        return right ? r.right - 1 : r.left + 1;
    };
    auto wheel = [this, tabPoint](int delta) {   // the mouse wheel turned with the cursor over the tab bar
        POINTL pt = tabPoint(0);
        SendMessageW(tabBar_, WM_MOUSEWHEEL, MAKEWPARAM(0, delta), MAKELPARAM(pt.x, pt.y));
    };
    add(L"pressing a tab makes it current at once, and dragging it along the bar moves it past the others "
        L"(a slip just into the next tab moves nothing)",
        [=, this] {
            newTab(path(L"sub"), false);
            newTab(L"", false);   // tabs: the test folder, sub, This PC
            dragAlongBar(0, tabEdge(1, false));   // a slip during a click, short of the next tab's middle
            dragAlongBar(1, tabEdge(2, true));
        },
        [=, this] {
            return tabs_.size() == 3 && samePath(tabs_[0].path, dir) && tabs_[1].path.empty() && active_ == 2 &&
                   TabCtrl_GetCurSel(tabBar_) == 2 && at(path(L"sub"));
        });
    add(L"dragging it back to the start puts it first", [=, this] { dragAlongBar(2, tabEdge(0, false)); },
        [=, this] {
            return samePath(tabs_[1].path, dir) && tabs_[2].path.empty() && active_ == 0 &&
                   TabCtrl_GetCurSel(tabBar_) == 0 && at(path(L"sub"));
        });
    add(L"the mouse wheel over the tabs shows the next tab (a touchpad's small steps add up) and stops at the last",
        [=, this] {
            for (int i = 0; i < 4; ++i) wheel(-30);   // four small steps make one
            wheel(-120);
            wheel(-120);   // past the last tab: nothing
        },
        [=, this] { return idle() && active_ == 2 && at(L""); });
    add(L"turning the wheel the other way goes back to the first tab, and stops there",
        [=, this] { for (int i = 0; i < 3; ++i) wheel(120); },
        [=, this] { return idle() && active_ == 0 && at(path(L"sub")); });
    add(L"the current tab's name, and only its, starts with a bullet", nullptr, [=, this] {
        for (int i = 0; i < int(tabs_.size()); ++i)
            if ((caption(i).rfind(L"• ", 0) == 0) != (i == active_)) return false;
        return true;
    });
    add(L"a new tab goes at the end, so the newest is the last one", [=, this] { selfTestKey('T', true); },
        [=, this] {
            return tabs_.size() == 4 && samePath(tabs_[1].path, dir) && tabs_[2].path.empty() && active_ == 3 &&
                   at(path(L"sub"));
        });
    add(L"(close the other tabs)", [=, this] { closeOtherTabs(1); },   // keeps the test folder's own tab
        [=, this] { return idle() && tabs_.size() == 1 && at(dir); });
    add(L"a lone tab is 150 wide at 100% scaling", nullptr, [=, this] { return tabWidth() == scale(150, dpi_); });
    std::wstring longName = path(L"a folder with a name too long for its tab");   // never opened: only its caption
    auto openTabsPast = [=, this](int width) {   // background tabs until more are open than fit at that width
        RECT bar{};
        GetClientRect(tabBar_, &bar);
        while (int(tabs_.size()) <= bar.right / width) newTab(longName, false);
    };
    add(L"when more tabs are open than fit that wide, all get narrower alike, and all still show",
        [=, this] { openTabsPast(scale(150, dpi_)); },
        [=, this] {
            RECT bar{}, last{};
            GetClientRect(tabBar_, &bar);
            TabCtrl_GetItemRect(tabBar_, int(tabs_.size()) - 1, &last);
            int width = tabWidth();
            selfTest_->note = std::to_wstring(tabs_.size()) + L" tabs " + std::to_wstring(width) + L" wide";
            return width >= scale(80, dpi_) && width < scale(150, dpi_) && last.right <= bar.right;
        });
    add(L"...but no narrower than 80; with more, the bar scrolls", [=, this] { openTabsPast(scale(80, dpi_)); },
        [=, this] {
            HWND arrows = FindWindowExW(tabBar_, nullptr, UPDOWN_CLASSW, nullptr);
            return tabWidth() == scale(80, dpi_) && arrows && IsWindowVisible(arrows);
        });
    add(L"(close them)", [=, this] { closeOtherTabs(active_); }, [=, this] { return idle() && tabs_.size() == 1 && at(dir); });
    add(L"a tab the control selects by itself (as assistive tools select one) switches to its folder",
        [=, this] {
            newTab(path(L"sub"), false);        // a second tab, right of this one
            TabCtrl_SetCurFocus(tabBar_, 1);   // on plain tabs this selects, and notifies the window as a click would
        },
        [=, this] { return idle() && active_ == 1 && at(path(L"sub")); });
    add(L"(close that tab)", [=, this] { closeTab(1); }, [=, this] { return idle() && tabs_.size() == 1 && at(dir); });
    // What Windows sends when the window reaches a monitor with another scaling: the dots per inch there, and where
    // the window goes.
    static UINT dpi0 = 0;
    static int row0 = 0, col0 = 0;
    static RECT at0{};
    auto dpiChange = [this](UINT dpi, RECT place) {
        SendMessageW(wnd_, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&place));
    };
    add(L"a move to a monitor with another scaling (as Windows reports it) rescales the font, the columns and the tabs",
        [=, this] {
            dpi0 = dpi_;
            row0 = rowHeight_;
            col0 = list_.columnWidths()[0];
            GetWindowRect(wnd_, &at0);
            RECT bigger{at0.left, at0.top, at0.left + (at0.right - at0.left) * 4 / 3, at0.top + (at0.bottom - at0.top) * 4 / 3};
            dpiChange(dpi0 * 4 / 3, bigger);
        },
        [=, this] {
            UINT dpi = dpi0 * 4 / 3;
            return dpi_ == dpi && rowHeight_ > row0 && list_.columnWidths()[0] == MulDiv(col0, int(dpi), int(dpi0)) &&
                   tabWidth() == scale(150, dpi);
        });
    add(L"...and the move back restores them", [=, this] { dpiChange(dpi0, at0); },
        [=, this] {
            int column = list_.columnWidths()[0] - col0;   // within rounding
            return dpi_ == dpi0 && rowHeight_ == row0 && column >= -1 && column <= 1 && tabWidth() == scale(150, dpi0);
        });
    // The test must not switch the user's theme: it applies the colours for one directly.
    add(L"in a high-contrast theme the window, list and tree take the theme's colours; the soft gray comes back after",
        [=, this] {
            applyColors(true);
            COLORREF window = GetSysColor(COLOR_WINDOW);
            bool themed = ListView_GetBkColor(list_.hwnd()) == window && ListView_GetTextBkColor(list_.hwnd()) == window &&
                          TreeView_GetBkColor(tree_.hwnd()) == window &&
                          HBRUSH(GetClassLongPtrW(wnd_, GCLP_HBRBACKGROUND)) == GetSysColorBrush(COLOR_WINDOW);
            selfTest_->note = themed ? L"theme colours" : L"still gray";
            applyColors(false);
        },
        [=, this] {
            COLORREF gray = RGB(235, 235, 235);
            return selfTest_->note == L"theme colours" && ListView_GetBkColor(list_.hwnd()) == gray &&
                   TreeView_GetBkColor(tree_.hwnd()) == gray;
        });
    add(L"switching tabs shows the new tab at once; nothing acts on the old one meanwhile",
        [=, this] {
            newTab(path(L"sub"), false);
            SetFocus(list_.hwnd());
            switchTab(active_ + 1);
            // Before "sub" can have loaded: the address bar names it, and Ctrl+Shift+N must not hit the old tab.
            wchar_t address[MAX_PATH] = {};
            GetWindowTextW(address_, address, MAX_PATH);
            selfTest_->note = samePath(address, path(L"sub")) ? L"address bar switched" : L"old tab still shown";
            selfTestKey('N', true, true);
        },
        [=, this] {
            return at(path(L"sub")) && selfTest_->note == L"address bar switched" && !exists(path(L"New folder")) &&
                   !exists(path(L"sub\\New folder"));
        });
    add(L"(close that tab)", [=, this] { closeTab(active_); }, [=, this] { return idle() && tabs_.size() == 1 && at(dir); });
    add(L"a typed path that fails while a rename box is open leaves the box alone",
        [=, this] {
            // The failure lands after the rename box opens (a dead share fails a minute later).
            goTo(L"C:\\no\\such\\folder\\anywhere");
            list_.select({L"file1.txt"}, L"file1.txt");
            selfTestKey(VK_F2);
        },
        [=, this] { return at(dir) && list_.renaming() && GetFocus() != address_; });
    add(L"a refresh asked to select an item, landing during a rename, keeps that for later",
        [=, this] {
            ListView_CancelEditLabel(list_.hwnd());   // the box from the step before
            SetFocus(list_.hwnd());
            list_.select({L"file1.txt"}, L"file1.txt");
            refresh(ViewState::selecting(L"added.txt"));   // as after a rename or a new folder; the load is in flight
            selfTestKey(VK_F2);                             // the box opens before it lands
        },
        [=, this] { return idle() && list_.renaming(); });
    add(L"...and once the box closes the queued refresh selects it",
        [=, this] { ListView_CancelEditLabel(list_.hwnd()); },
        [=, this] {
            selfTest_->note = L"focused " + list_.focusedName();
            return idle() && !refreshQueued_ && list_.focusedName() == L"added.txt";
        });
    add(L"a newer request to select an item replaces an older one still waiting",
        [=, this] {
            queuedView_ = {dir, ViewState::selecting(L"file1.txt")};   // an older request, waiting (a slow folder)
            refresh(ViewState::selecting(L"added.txt"));               // then a newer one, as after a second rename
        },
        [=, this] { return idle() && list_.focusedName() == L"added.txt"; });
    add(L"...so the waiting refresh, when it runs, leaves the newer item selected",
        [=, this] { SendMessageW(wnd_, WM_TIMER, kRefreshTimer, 0); },
        [=, this] {
            selfTest_->note = L"focused " + list_.focusedName();
            return idle() && list_.focusedName() == L"added.txt";
        });
    add(L"a command that does not exist says Can't find",
        [=, this] { goTo(L"no-such-program-qoqdir"); },
        [=, this] {
            DWORD from = 0, to = 0;   // the text stays selected, so it can be corrected (5.3)
            SendMessageW(address_, EM_GETSEL, WPARAM(&from), LPARAM(&to));
            return at(dir) && statusMessage().find(L"Can't find") != std::wstring::npos && from == 0 &&
                   int(to) == GetWindowTextLengthW(address_);
        });
    add(L"a program typed by name is Windows' own, not a file of that name in the folder shown",
        [=, this] {
            writeFile(path(L"rundll32.exe"));   // not a program: run from here, it would fail
            setMessage(L"");
            goTo(L"rundll32");
            DeleteFileW(path(L"rundll32.exe").c_str());
        },
        [=, this] {
            selfTest_->note = L"status: " + statusMessage();
            return at(dir) && statusMessage().empty();
        });

    add(L"(open This PC)", [=, this] { open(L"", Nav::New); }, [=, this] { return at(L""); });
    add(L"typing . on This PC says Can't find (not handed to Windows, which opens Explorer)",
        [=, this] { setMessage(L""); goTo(L"."); },
        [=, this] {
            selfTest_->note = L"status: " + statusMessage();
            return at(L"") && statusMessage().find(L"Can't find") != std::wstring::npos;
        });
    backHome(L"(back to the test folder from This PC)");

    add(L"typing a folder path opens it", [=, this] { goTo(path(L"sub")); }, [=, this] { return at(path(L"sub")); });
    add(L"typing an unreachable network path does not freeze the window",
        [=, this] {
            SetWindowTextW(address_, L"\\\\192.0.2.1\\share");   // reserved test address: nothing answers
            SetFocus(address_);
            t0 = nowMs();
            selfTestKey(VK_RETURN);
            keyMs = nowMs() - t0;
            selfTest_->note = L"Enter took " + std::to_wstring(int(keyMs)) + L" ms";
        },
        [=, this] { return keyMs < 500; });
    add(L"...and opening another folder cancels it", [=, this] { open(dir, Nav::New); }, [=, this] { return at(dir); }, 5000, true);
    add(L"typing a subfolder's name opens it here (not in Explorer)", [=, this] { goTo(L"sub"); },
        [=, this] { return at(path(L"sub")); });
    add(L"typing ..\\ goes up", [=, this] { goTo(L"..\\"); }, [=, this] { return at(dir); });
    add(L"typing .\\sub opens it here (not in Explorer)", [=, this] { goTo(L".\\sub"); },
        [=, this] { return at(path(L"sub")); });
    add(L"a long folder name of emojis is cut to fit its tab, ending in an ellipsis, with no broken character",
        [=, this] {
            std::wstring name;
            for (int i = 0; i < 40; ++i) name += L"\U0001F600";   // each is two UTF-16 units; the cut lands among them
            CreateDirectoryW(path(name).c_str(), nullptr);
            open(path(name), Nav::New);
        },
        [=, this] {
            if (!idle() || leafName(tab().path).size() != 80) return false;
            std::wstring shown = caption(active_);
            for (size_t i = 0; i < shown.size(); ++i)   // every high surrogate must be followed by its low one
                if (IS_HIGH_SURROGATE(shown[i]) && (i + 1 >= shown.size() || !IS_LOW_SURROGATE(shown[i + 1])))
                    return false;
            return shown.size() < 80 && shown.back() == L'…';
        });
    backHome(L"(back up to the test folder)");

    // A folder whose name looks like an environment variable: only what the user types is expanded.
    add(L"(open a folder literally named %TEMP%, with sub and other in it)",
        [=, this] {
            for (const wchar_t* name : {L"%TEMP%", L"%TEMP%\\sub", L"%TEMP%\\other"})
                CreateDirectoryW(path(name).c_str(), nullptr);
            open(path(L"%TEMP%"), Nav::New);
        },
        [=, this] { return at(path(L"%TEMP%")) && list_.count() == 2; });
    add(L"typing a subfolder's name in a folder named %TEMP% opens it (the folder's own path is not expanded)",
        [=, this] { goTo(L"sub"); },
        [=, this] {
            selfTest_->note = L"tab at " + tab().path + L", status: " + statusMessage();
            return at(path(L"%TEMP%\\sub"));
        });
    add(L"(back to the %TEMP% folder)", [=, this] { open(path(L"%TEMP%"), Nav::New); },
        [=, this] { return at(path(L"%TEMP%")) && list_.count() == 2; });
    add(L"Enter in the filter box opens only the highlighted match, not every selected one (5.4)",
        [=, this] {
            SetFocus(list_.hwnd());
            list_.selectAll();      // both folders selected, "sub" highlighted (it was just left)
            selfTestType(L"*");     // a filter both still match
            selfTestKey(VK_RETURN);
        },
        [=, this] {
            selfTest_->note = L"tab at " + tab().path + L", " + std::to_wstring(tabs_.size()) + L" tab(s)";
            return at(path(L"%TEMP%\\sub")) && tabs_.size() == 1;
        });
    add(L"(close any tabs that opened)",
        [=, this] { while (tabs_.size() > 1) closeTab(int(tabs_.size()) - 1); },
        [=, this] { return idle() && tabs_.size() == 1; });
    backHome(L"(back to the test folder)");
    add(L"(remove the %TEMP% folder)",
        [=, this] {
            for (const wchar_t* name : {L"%TEMP%\\sub", L"%TEMP%\\other", L"%TEMP%"})
                RemoveDirectoryW(path(name).c_str());
            refresh();
        },
        [=, this] { return at(dir) && !list_.hasFolder(L"%TEMP%"); });

    // Reliability plan: hidden items, the filter during a load, a refresh queued before a failure.
    add(L"(add a hidden file and a hidden folder)",
        [=, this] {
            writeFile(path(L"secret.txt"));
            CreateDirectoryW(path(L"hid").c_str(), nullptr);
            for (const wchar_t* name : {L"secret.txt", L"hid"})
                SetFileAttributesW(path(name).c_str(), GetFileAttributesW(path(name).c_str()) | FILE_ATTRIBUTE_HIDDEN);
            refresh();
        },
        [=, this] { return idle() && !listed(L"secret.txt") && !listed(L"hid"); });
    auto loads = std::make_shared<UINT>(0);
    add(L"Ctrl+H shows hidden files at once, without reading the folder again",
        [=, this] {
            *loads = generation_;
            SetFocus(list_.hwnd());
            selfTestKey('H', true);
        },
        [=, this] {
            selfTest_->note = std::to_wstring(generation_ - *loads) + L" read(s)";
            return idle() && listed(L"secret.txt") && generation_ == *loads;
        });
    add(L"(Ctrl+H hides them again)", [=, this] { selfTestKey('H', true); },
        [=, this] { return idle() && !listed(L"secret.txt"); });
    add(L"typing a hidden folder's name opens it here (not in Explorer)",
        [=, this] { goTo(L"hid"); },
        [=, this] { return at(path(L"hid")); });
    add(L"letters typed while a folder loads stay in the filter box when it lands (5.4)",
        [=, this] {
            open(dir, Nav::New);   // back up from "hid"
            SetFocus(list_.hwnd());
            selfTestType(L"file");
        },
        [=, this] {
            selfTest_->note = L"filter \"" + windowText(filter_) + L"\"";
            return at(dir) && windowText(filter_) == L"file";
        });
    add(L"a folder that fails to open leaves the filter of the folder still shown",
        [=, this] { open(path(L"no-such-folder"), Nav::New); },
        [=, this] {
            selfTest_->note = L"filter \"" + windowText(filter_) + L"\"";
            return at(dir) && windowText(filter_) == L"file";
        });
    add(L"a refresh queued just before a tab became not available waits for F5 (4.3)",
        [=, this] {
            queueRefresh();   // a change in the folder, just before
            Pending p;
            p.nav = Nav::Refresh;
            p.tabId = tab().id;
            onLoadFailed(Listing{dir, {}, ERROR_BAD_NETPATH, false}, p);   // then it became unreachable
            SendMessageW(wnd_, WM_TIMER, kRefreshTimer, 0);                 // and the queued refresh fires
        },
        [=, this] {
            return idle() && tab().unavailable && statusMessage().find(L"not available") != std::wstring::npos;
        });
    add(L"(F5 shows it again)", [=, this] { selfTestKey(VK_F5); }, [=, this] { return at(dir) && !tab().unavailable; });

    // History entries for deleted folders, and shortcuts.
    add(L"(open a folder that will be deleted)",
        [=, this] { CreateDirectoryW(path(L"temp1").c_str(), nullptr); open(path(L"temp1"), Nav::New); },
        [=, this] { return at(path(L"temp1")); });
    backHome(L"(go back to the test folder)");
    add(L"Back to a deleted folder drops it from history instead of getting stuck",
        [=, this] { RemoveDirectoryW(path(L"temp1").c_str()); selfTestKey(VK_LEFT, false, false, true); },
        [=, this] { return at(dir) && (tab().back.empty() || !samePath(tab().back.back(), path(L"temp1"))); });
    add(L"a shortcut to a folder opens it here",
        [=, this] {
            makeShortcut(path(L"sub-link.lnk"), path(L"sub"));
            refresh(ViewState::selecting(L"sub-link.lnk"));
        },
        [=, this] {
            if (!idle() || list_.focusedName() != L"sub-link.lnk") return false;
            SetFocus(list_.hwnd());
            selfTestKey(VK_RETURN);
            return true;
        });
    add(L"(the shortcut's folder is shown)", nullptr, [=, this] { return at(path(L"sub")); });
    backHome(L"(back up to the test folder)");
    add(L"a shortcut whose target ends with a backslash opens the folder with a clean path",
        [=, this] {
            makeShortcut(path(L"slash.lnk"), path(L"sub") + L"\\");   // as hand-made shortcuts often have it
            openShortcut(path(L"slash.lnk"), false);
        },
        [=, this] { return at(path(L"sub")) && tab().path.back() != L'\\'; });   // else the caption and Up break
    backHome(L"(back up to the test folder)");
    add(L"Ctrl+Enter on a shortcut to a folder opens it in a new background tab (not in Explorer)",
        [=, this] {
            list_.select({L"sub-link.lnk"}, L"sub-link.lnk");
            SetFocus(list_.hwnd());
            selfTestKey(VK_RETURN, true);
        },
        [=, this] { return tabs_.size() == 2 && samePath(tabs_[size_t(active_) + 1].path, path(L"sub")) && at(dir); });
    add(L"(close that tab)", [=, this] { closeTab(active_ + 1); }, [=, this] { return idle() && tabs_.size() == 1; });
    add(L"Enter on two shortcuts to folders, one whose target ends with a backslash, opens both in background tabs",
        [=, this] {
            makeShortcut(path(L"slash2.lnk"), path(L"sub") + L"\\");
            refresh(ViewState{{L"sub-link.lnk", L"slash2.lnk"}, L"sub-link.lnk"});
        },
        [=, this] {
            if (!idle() || list_.selection().size() != 2) return false;
            SetFocus(list_.hwnd());
            selfTestKey(VK_RETURN);
            return true;
        });
    add(L"(both are in tabs behind the test folder, with clean paths, so with captions)", nullptr, [=, this] {
        if (tabs_.size() != 3 || !at(dir)) return false;
        selfTest_->note = L"tab paths: " + tabs_[1].path + L" | " + tabs_[2].path;
        return samePath(tabs_[1].path, path(L"sub")) && samePath(tabs_[2].path, path(L"sub"));   // not "sub\"
    });
    add(L"(close those tabs)", [=, this] { closeTab(1); closeTab(1); },
        [=, this] { return idle() && tabs_.size() == 1 && at(dir); });
    add(L"in a network folder that is not available, paste, new folder and a drag over its tab return at once",
        [=, this] {
            // As if this tab's folder had just failed to load (a real failure can take minutes to arrive).
            tab().path = L"\\\\192.0.2.1\\share";
            Pending p;
            p.nav = Nav::Refresh;
            p.tabId = tab().id;
            onLoadFailed(Listing{tab().path, {}, ERROR_BAD_NETPATH, false}, p);
            POINTL cursor = tabPoint(active_);
            Com<IDataObject> data = dataObject(wnd_, path(L"file1.txt"));
            DWORD effect = DROPEFFECT_COPY;
            SetFocus(list_.hwnd());
            t0 = nowMs();
            selfTestKey('V', true);
            selfTestKey('N', true, true);
            tabDrop_->DragEnter(data.get(), MK_LBUTTON, cursor, &effect);
            tabDrop_->DragLeave();
            keyMs = nowMs() - t0;
            selfTest_->note = L"took " + std::to_wstring(int(keyMs)) + L" ms";
        },
        [=, this] { return keyMs < 100; });
    add(L"(back to the test folder)", [=, this] { tab().path = dir; open(dir, Nav::Refresh); }, [=, this] { return at(dir); });
    add(L"a shortcut to a folder that is gone says so here (no Windows dialog)",
        [=, this] {
            makeShortcut(path(L"gone.lnk"), path(L"no-such-folder"));
            refresh(ViewState::selecting(L"gone.lnk"));
        },
        [=, this] {
            if (!idle() || list_.focusedName() != L"gone.lnk") return false;
            SetFocus(list_.hwnd());
            selfTestKey(VK_RETURN);
            return true;
        });
    add(L"(the status bar names the missing folder, and the tab stays put)", nullptr,
        [=, this] { return at(dir) && statusMessage().find(L"no-such-folder") != std::wstring::npos; });
    add(L"a shortcut to This PC opens This PC here (not in Explorer)",
        [=, this] {
            makeShortcut(path(L"pc.lnk"), L"");
            refresh(ViewState::selecting(L"pc.lnk"));
        },
        [=, this] {
            if (!idle() || list_.focusedName() != L"pc.lnk") return false;
            SetFocus(list_.hwnd());
            selfTestKey(VK_RETURN);
            return true;
        });
    add(L"(This PC is shown, with its drives)", nullptr, [=, this] { return at(L"") && list_.count() > 0; });
    backHome(L"(back to the test folder)");

    // Regression tests for bugs fixed earlier.
    add(L"holding F5 while a folder loads does not start more loads",
        [=, this] {
            UINT before = generation_;
            open(path(L"sub"), Nav::New);
            SetFocus(list_.hwnd());
            for (int i = 0; i < 20; ++i) selfTestKey(VK_F5);   // like a held-down key
            selfTest_->note = std::to_wstring(generation_ - before) + L" load(s) started";
        },
        [=, this] { return at(path(L"sub")) && selfTest_->note == L"1 load(s) started"; });
    backHome(L"(back up to the test folder again)");
    add(L"a background refresh never cancels a folder being opened",
        [=, this] {
            open(path(L"sub"), Nav::New);
            SendMessageW(wnd_, WM_TIMER, kRefreshTimer, 0);   // auto-refresh fires while "sub" is loading
        },
        [=, this] { return at(path(L"sub")); });
    add(L"\"Loading...\" goes away after a slow refresh",
        [=, this] {
            refresh();
            SendMessageW(wnd_, WM_TIMER, kLoadingTimer, 0);   // as if the refresh took over 250 ms
        },
        [=, this] { return idle() && statusMessage() != L"Loading…"; });
    add(L"back up to the test folder", [=, this] { SetFocus(list_.hwnd()); selfTestKey(VK_BACK); }, [=, this] { return at(dir); });
    add(L"(open a folder two levels down)",
        [=, this] { CreateDirectoryW(path(L"sub\\deep").c_str(), nullptr); open(path(L"sub\\deep"), Nav::New); },
        [=, this] { return at(path(L"sub\\deep")); });
    add(L"a removed drive is noticed by a tab two folders deep (shown as not available)",
        [=, this] {
            // Two levels down only WM_DEVICECHANGE tells: drop the watch, remove the folder, send it.
            watch_ = {};
            RemoveDirectoryW(path(L"sub\\deep").c_str());
            DEV_BROADCAST_VOLUME vol{};
            vol.dbcv_size = sizeof vol;
            vol.dbcv_devicetype = DBT_DEVTYP_VOLUME;
            SendMessageW(wnd_, WM_DEVICECHANGE, DBT_DEVICEREMOVECOMPLETE, reinterpret_cast<LPARAM>(&vol));
        },
        [=, this] {
            return at(path(L"sub\\deep")) && list_.count() == 0 &&
                   statusMessage().find(L"not available") != std::wstring::npos;
        });
    add(L"(back to the test folder)", [=, this] { tab().path = dir; open(dir, Nav::Refresh); }, [=, this] { return at(dir); });

    // Slow lookups happen on worker threads (so network drives cannot freeze the window).
    add(L"a program's own icon arrives from the background",
        [=, this] {
            wchar_t cmd[MAX_PATH] = {};
            GetSystemDirectoryW(cmd, MAX_PATH);
            CopyFileW(joinPath(cmd, L"cmd.exe").c_str(), path(L"tool.exe").c_str(), FALSE);
            refresh();
        },
        [=, this] {
            SHFILEINFOW sfi{};
            SHGetFileInfoW(path(L"tool.exe").c_str(), 0, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
            for (int row = 0; row < list_.count(); ++row) {
                if (list_.at(row).name != L"tool.exe") continue;
                LVITEMW item{};
                item.mask = LVIF_IMAGE;
                item.iItem = row;
                ListView_GetItem(list_.hwnd(), &item);
                return idle() && item.iImage == sfi.iIcon;
            }
            return false;
        });
    add(L"sorting by Type puts the Type column in order",
        [=, this] {
            makeShortcut(path(L"sub-link2.lnk"), path(L"sub"));   // a few different types
            writeFile(path(L"archive.zip"));
            writeFile(path(L"page.html"));
            refresh();
        },
        [=, this] {
            if (!idle() || list_.total() < 8) return false;
            list_.setSort(SortKey::Type, true);
            bool ordered = true;
            for (int row = 0; row + 1 < list_.count(); ++row) {
                const Item &a = list_.at(row), &b = list_.at(row + 1);
                if (a.isDir() == b.isDir() && StrCmpLogicalW(typeName(a).c_str(), typeName(b).c_str()) > 0) ordered = false;
            }
            list_.setSort(SortKey::Name, true);
            return ordered;
        });
    add(L"the Bookmarks folder is created and shown at the top of the tree", nullptr, [=, this] {
        TVITEMW item{};
        item.mask = TVIF_TEXT;
        wchar_t text[64] = {};
        item.pszText = text;
        item.cchTextMax = 64;
        item.hItem = TreeView_GetRoot(tree_.hwnd());
        TreeView_GetItem(tree_.hwnd(), &item);
        return isDirectory(path(L"Bookmarks")) && std::wstring(text) == L"Bookmarks";
    });
    add(L"right-click menus can be built for This PC, a drive and a file", nullptr, [=, this] {
        return itemMenu(wnd_, L"") && itemMenu(wnd_, L"C:\\") && itemMenu(wnd_, path(L"file1.txt"));
    });
    add(L"a drag over a tab whose folder was never shown is refused at once (nobody has checked that folder)",
        [=, this] {
            newTab(path(L"made"), false);   // a second tab, right of the current one, as from --tab= or a middle-click
            POINTL cursor = tabPoint(active_ + 1);
            Com<IDataObject> data = dataObject(wnd_, path(L"file1.txt"));
            dragEffect = DROPEFFECT_COPY;
            t0 = nowMs();
            tabDrop_->DragEnter(data.get(), MK_LBUTTON, cursor, &dragEffect);
            DWORD over = DROPEFFECT_COPY;
            tabDrop_->DragOver(MK_LBUTTON, cursor, &over);
            dragEffect |= over;   // refused as the drag enters, and as it moves
            tabDrop_->DragLeave();
            keyMs = nowMs() - t0;
            selfTest_->note = L"took " + std::to_wstring(int(keyMs)) + L" ms";
        },
        [=, this] { return dragEffect == DROPEFFECT_NONE && keyMs < 100; });
    add(L"(show that tab once)", [=, this] { switchTab(active_ + 1); }, [=, this] { return at(path(L"made")); });
    add(L"(and come back)", [=, this] { switchTab(active_ - 1); }, [=, this] { return at(dir); });
    add(L"dropping a file on a tab puts it in that tab's folder; while the file is over it, that tab shows as the "
        L"current one, and afterwards the real one does again",
        [=, this] {
            POINTL cursor = tabPoint(active_ + 1);
            Com<IDataObject> data = dataObject(wnd_, path(L"file1.txt"));   // what a drag carries
            DWORD effect = DROPEFFECT_COPY;
            tabDrop_->DragEnter(data.get(), MK_LBUTTON, cursor, &effect);
            effect = DROPEFFECT_COPY;
            tabDrop_->DragOver(MK_LBUTTON, cursor, &effect);
            selfTest_->note = TabCtrl_GetCurSel(tabBar_) == active_ + 1 ? L"shown while over it" : L"not shown while over it";
            effect = DROPEFFECT_COPY;
            tabDrop_->Drop(data.get(), MK_LBUTTON, cursor, &effect);
        },
        [=, this] {
            return exists(path(L"made\\file1.txt")) && dropTab_ == -1 && selfTest_->note == L"shown while over it" &&
                   TabCtrl_GetCurSel(tabBar_) == active_;
        },
        10000);
    add(L"(close the extra tab)", [=, this] { closeTab(active_ + 1); }, [=, this] { return idle() && tabs_.size() == 1; });
    add(L"middle-click on a folder opens it in a new background tab",
        [=, this] {
            for (int row = 0; row < list_.count(); ++row) {
                if (list_.at(row).name != L"sub") continue;
                RECT r{};
                ListView_GetItemRect(list_.hwnd(), row, &r, LVIR_LABEL);
                SendMessageW(list_.hwnd(), WM_MBUTTONUP, 0, MAKELPARAM((r.left + r.right) / 2, (r.top + r.bottom) / 2));
            }
        },
        [=, this] { return tabs_.size() == 2 && samePath(tabs_[size_t(active_) + 1].path, path(L"sub")) && at(dir); });
    add(L"(close that tab)", [=, this] { closeTab(active_ + 1); }, [=, this] { return idle() && tabs_.size() == 1; });
    add(L"middle-click in the tree opens a new background tab",
        [=, this] {
            RECT r{};
            TreeView_GetItemRect(tree_.hwnd(), TreeView_GetNextSibling(tree_.hwnd(), TreeView_GetRoot(tree_.hwnd())), &r, TRUE);   // Desktop (after Bookmarks)
            SendMessageW(tree_.hwnd(), WM_MBUTTONUP, 0, MAKELPARAM((r.left + r.right) / 2, (r.top + r.bottom) / 2));
        },
        [=, this] {
            return tabs_.size() == 2 && samePath(tabs_[size_t(active_) + 1].path, knownFolder(FOLDERID_Desktop)) && at(dir);
        });
    add(L"(close that tab too)", [=, this] { closeTab(active_ + 1); }, [=, this] { return idle() && tabs_.size() == 1; });
    add(L"the tree lists the drives under This PC", nullptr, [=, this] {
        HTREEITEM node = TreeView_GetRoot(tree_.hwnd()), last = nullptr;
        for (; node; node = TreeView_GetNextSibling(tree_.hwnd(), node)) last = node;
        return last && TreeView_GetChild(tree_.hwnd(), last) != nullptr;
    });
    add(L"root tree nodes have an expand arrow", nullptr, [=, this] {
        // The arrow sits in the indent left of the icon; TVS_HASBUTTONS gives roots one only with TVS_LINESATROOT.
        RECT r{};
        TreeView_GetItemRect(tree_.hwnd(), TreeView_GetRoot(tree_.hwnd()), &r, FALSE);   // the whole Bookmarks row
        TVHITTESTINFO hit{};
        hit.pt = {r.left + int(TreeView_GetIndent(tree_.hwnd())) / 2, (r.top + r.bottom) / 2};
        TreeView_HitTest(tree_.hwnd(), &hit);
        return (hit.flags & TVHT_ONITEMBUTTON) != 0;
    });
    add(L"expanding a tree folder lists its subfolders",
        [=, this] { TreeView_Expand(tree_.hwnd(), TreeView_GetNextSibling(tree_.hwnd(), TreeView_GetRoot(tree_.hwnd())), TVE_EXPAND); },   // Desktop (after Bookmarks)
        [=, this] {
            int expected = 0;
            for (const Item& it : listFolder(knownFolder(FOLDERID_Desktop), false).items) expected += it.isDir();
            HTREEITEM root = TreeView_GetNextSibling(tree_.hwnd(), TreeView_GetRoot(tree_.hwnd()));   // Desktop
            int shown = 0;
            for (HTREEITEM c = TreeView_GetChild(tree_.hwnd(), root); c; c = TreeView_GetNextSibling(tree_.hwnd(), c)) ++shown;
            TVITEMW item{};
            item.mask = TVIF_CHILDREN;
            item.hItem = root;
            TreeView_GetItem(tree_.hwnd(), &item);
            // Done when the subfolders are shown, or the node knows it has none.
            return expected > 0 ? shown == expected : item.cChildren == 0;
        });
    auto nodeTexts = [this](HTREEITEM first) {   // the texts of a node and its next siblings, comma-separated
        std::wstring texts;
        for (HTREEITEM c = first; c; c = TreeView_GetNextSibling(tree_.hwnd(), c)) {
            wchar_t text[64] = {};
            TVITEMW item{};
            item.mask = TVIF_TEXT;
            item.hItem = c;
            item.pszText = text;
            item.cchTextMax = 64;
            TreeView_GetItem(tree_.hwnd(), &item);
            texts += (texts.empty() ? L"" : L",") + std::wstring(text);
        }
        return texts;
    };
    auto bookmarkNodes = [=, this] {   // the texts under Bookmarks, the tree's first root
        return nodeTexts(TreeView_GetChild(tree_.hwnd(), TreeView_GetRoot(tree_.hwnd())));
    };
    auto rootTexts = [=, this] { return nodeTexts(TreeView_GetRoot(tree_.hwnd())); };
    auto twoRoots = std::make_shared<bool>(false);
    add(L"the user folders arrive from a worker, between Bookmarks and This PC",
        [=, this] {
            tree_.fill();   // as at startup and on a drive change
            // Nothing asks Windows about the user folders here (4.1): they arrive by message, later.
            *twoRoots = rootTexts() == L"Bookmarks,This PC";
        },
        [=, this] {
            SHFILEINFOW sfi{};
            SHGetFileInfoW(knownFolder(FOLDERID_Desktop).c_str(), 0, &sfi, sizeof sfi, SHGFI_DISPLAYNAME);
            std::wstring roots = rootTexts();
            return *twoRoots && roots.starts_with(L"Bookmarks," + std::wstring(sfi.szDisplayName) + L",") &&
                   roots.ends_with(L",This PC");
        });
    add(L"an empty Bookmarks node keeps its arrow and lists a bookmark added later",
        [=, this] {
            CreateDirectoryW(path(L"Bookmarks\\one").c_str(), nullptr);   // the startup read found Bookmarks empty
            TreeView_SelectItem(tree_.hwnd(), TreeView_GetRoot(tree_.hwnd()));
            SendMessageW(tree_.hwnd(), WM_KEYDOWN, VK_RIGHT, 0);   // expand, as the user would
        },
        [=] { return bookmarkNodes() == L"one"; });
    add(L"Bookmarks shows its folders and shortcuts in the tree, open from the start",
        [=, this] {
            CreateDirectoryW(path(L"Bookmarks\\one").c_str(), nullptr);
            makeShortcut(path(L"Bookmarks\\sub.lnk"), path(L"sub"));
            tree_.fill();   // as at startup
        },
        [=, this] {
            if (bookmarkNodes() != L"one,sub") return false;
            SHFILEINFOW own{};   // the shortcut's own icon arrives after its node (it may wait on the target)
            SHGetFileInfoW(path(L"Bookmarks\\sub.lnk").c_str(), 0, &own, sizeof own,
                           SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
            HWND tv = tree_.hwnd();
            TVITEMW item{};
            item.mask = TVIF_IMAGE;
            item.hItem = TreeView_GetNextSibling(tv, TreeView_GetChild(tv, TreeView_GetRoot(tv)));   // "sub"
            TreeView_GetItem(tv, &item);
            return item.iImage == own.iIcon;
        });
    add(L"choosing a bookmark shortcut in the tree opens its folder",
        [=, this] {
            TreeView_SelectItem(tree_.hwnd(), TreeView_GetChild(tree_.hwnd(), TreeView_GetRoot(tree_.hwnd())));   // "one"
            SendMessageW(tree_.hwnd(), WM_KEYDOWN, VK_DOWN, 0);   // to "sub", as the user would
        },
        [=, this] { return at(path(L"sub")); });
    add(L"Rename on a tree folder's menu opens the rename box for it in the list",
        [=, this] { renameNode(path(L"sub")); },
        [=, this] { return at(dir) && list_.renaming() && list_.focusedName() == L"sub"; });
    add(L"collapsing and expanding a tree folder shows its new subfolders",
        [=, this] {
            ListView_CancelEditLabel(list_.hwnd());   // the box from the step before
            CreateDirectoryW(path(L"Bookmarks\\two").c_str(), nullptr);
            TreeView_SelectItem(tree_.hwnd(), TreeView_GetRoot(tree_.hwnd()));
            SendMessageW(tree_.hwnd(), WM_KEYDOWN, VK_LEFT, 0);    // collapse, as the user would
            SendMessageW(tree_.hwnd(), WM_KEYDOWN, VK_RIGHT, 0);   // and expand again
        },
        [=] { return bookmarkNodes() == L"one,two,sub"; });
    add(L"collapsing a node while a child is being read drops the late result",
        [=, this] {
            CreateDirectoryW(path(L"Bookmarks\\one\\deep").c_str(), nullptr);
            HWND tv = tree_.hwnd();
            TreeView_Expand(tv, TreeView_GetChild(tv, TreeView_GetRoot(tv)), TVE_EXPAND);   // "one": its read starts
            TreeView_SelectItem(tv, TreeView_GetRoot(tv));
            SendMessageW(tv, WM_KEYDOWN, VK_LEFT, 0);    // collapse Bookmarks: deletes "one" before its read lands
            SendMessageW(tv, WM_KEYDOWN, VK_RIGHT, 0);   // and read it fresh
        },
        [=, this] {
            // Both reads landed: no late "deep" under Bookmarks, and every node the tree counts is reachable.
            HWND tv = tree_.hwnd();
            if (GetTickCount64() - selfTest_->startTick < 400 || bookmarkNodes() != L"one,two,sub") return false;
            for (HTREEITEM c = TreeView_GetChild(tv, TreeView_GetRoot(tv)); c; c = TreeView_GetNextSibling(tv, c))
                if (TreeView_GetChild(tv, c)) return false;
            std::function<UINT(HTREEITEM)> count = [&](HTREEITEM n) {
                UINT k = 0;
                for (; n; n = TreeView_GetNextSibling(tv, n)) k += 1 + count(TreeView_GetChild(tv, n));
                return k;
            };
            return count(TreeView_GetRoot(tv)) == TreeView_GetCount(tv);
        });
    add(L"(expand a bookmark folder in the tree)",
        [=, this] {
            HWND tv = tree_.hwnd();
            TreeView_Expand(tv, TreeView_GetChild(tv, TreeView_GetRoot(tv)), TVE_EXPAND);   // "one"
        },
        [=, this] {
            HWND tv = tree_.hwnd();
            return TreeView_GetChild(tv, TreeView_GetChild(tv, TreeView_GetRoot(tv))) != nullptr;   // "deep" is shown
        });
    add(L"a drive coming or going re-reads This PC and leaves the rest of the tree as it was (4.5)",
        [=, this] {
            DEV_BROADCAST_VOLUME vol{};
            vol.dbcv_size = sizeof vol;
            vol.dbcv_devicetype = DBT_DEVTYP_VOLUME;
            SendMessageW(wnd_, WM_DEVICECHANGE, DBT_DEVICEARRIVAL, reinterpret_cast<LPARAM>(&vol));
        },
        [=, this] {
            HWND tv = tree_.hwnd();
            HTREEITEM one = TreeView_GetChild(tv, TreeView_GetRoot(tv)), last = TreeView_GetRoot(tv);
            for (HTREEITEM n = last; n; n = TreeView_GetNextSibling(tv, n)) last = n;   // This PC
            return bookmarkNodes() == L"one,two,sub" && one && TreeView_GetChild(tv, one) &&
                   (TreeView_GetItemState(tv, one, TVIS_EXPANDED) & TVIS_EXPANDED) && TreeView_GetChild(tv, last);
        });
    auto one = [this] { return TreeView_GetChild(tree_.hwnd(), TreeView_GetRoot(tree_.hwnd())); };   // open, with "deep"
    add(L"F5 reads the folders open in the tree again: one that is the same stays as it is (\"one\" stays open), one "
        L"that changed shows its new subfolder (5.5)",
        [=, this] {
            CreateDirectoryW(path(L"Bookmarks\\one\\new").c_str(), nullptr);
            selfTestKey(VK_F5);
        },
        [=, this] {
            return bookmarkNodes() == L"one,two,sub" && nodeTexts(TreeView_GetChild(tree_.hwnd(), one())) == L"deep,new" &&
                   (TreeView_GetItemState(tree_.hwnd(), one(), TVIS_EXPANDED) & TVIS_EXPANDED);
        });
    add(L"(the status bar says Refreshed)", nullptr, [=, this] { return statusMessage() == L"Refreshed"; });
    auto newNode = [=, this] { return TreeView_GetNextSibling(tree_.hwnd(), TreeView_GetChild(tree_.hwnd(), one())); };
    add(L"(open the folder new in the tree: it has no subfolders, so its arrow goes)",
        [=, this] { TreeView_Expand(tree_.hwnd(), newNode(), TVE_EXPAND); },
        [=, this] {
            TVITEMW item{};
            item.mask = TVIF_CHILDREN;
            item.hItem = newNode();
            TreeView_GetItem(tree_.hwnd(), &item);
            return item.cChildren == 0;
        });
    add(L"the Refresh button does the same, and reads again a folder known to have no subfolders: it shows the one "
        L"made since",
        [=, this] {
            CreateDirectoryW(path(L"Bookmarks\\one\\new\\inside").c_str(), nullptr);
            SendMessageW(refresh_, BM_CLICK, 0, 0);
        },
        [=, this] { return nodeTexts(TreeView_GetChild(tree_.hwnd(), newNode())) == L"inside"; });
    add(L"(open Bookmarks\\one in the list)", [=, this] { open(path(L"Bookmarks\\one"), Nav::New); },
        [=, this] { return at(path(L"Bookmarks\\one")); });
    add(L"a subfolder made in the folder shown shows up in the tree by itself: its open node is read again (5.5)",
        [=, this] { CreateDirectoryW(path(L"Bookmarks\\one\\later").c_str(), nullptr); },
        [=, this] { return nodeTexts(TreeView_GetChild(tree_.hwnd(), one())) == L"deep,later,new"; });
    add(L"a shortcut made in the folder shown shows up in the tree by itself too, after the subfolders, with its own icon "
        L"and no arrow (5.6)",
        [=, this] { makeShortcut(path(L"Bookmarks\\one\\sub.lnk"), path(L"sub")); },
        [=, this] {
            if (nodeTexts(TreeView_GetChild(tree_.hwnd(), one())) != L"deep,later,new,sub") return false;
            SHFILEINFOW own{};
            SHGetFileInfoW(path(L"Bookmarks\\one\\sub.lnk").c_str(), 0, &own, sizeof own,
                           SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
            HTREEITEM sub = TreeView_GetChild(tree_.hwnd(), one());   // the last child
            while (HTREEITEM after = TreeView_GetNextSibling(tree_.hwnd(), sub)) sub = after;
            TVITEMW item{};
            item.mask = TVIF_IMAGE | TVIF_CHILDREN;
            item.hItem = sub;
            TreeView_GetItem(tree_.hwnd(), &item);
            return item.iImage == own.iIcon && !item.cChildren;
        });
    backHome(L"(back to the test folder)");
    add(L"a folder deleted while the list shows another leaves the tree once its parent is read again, as it is when a "
        L"node's own menu closes",
        [=, this] {
            RemoveDirectoryW(path(L"Bookmarks\\one\\later").c_str());
            tree_.refreshFolder(path(L"Bookmarks\\one"));
        },
        [=, this] { return nodeTexts(TreeView_GetChild(tree_.hwnd(), one())) == L"deep,new,sub"; });
    auto nodePoint = [this](HTREEITEM node) {   // the middle of a tree node, in screen coordinates
        RECT r{};
        TreeView_GetItemRect(tree_.hwnd(), node, &r, TRUE);
        POINT pt{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        ClientToScreen(tree_.hwnd(), &pt);
        return POINTL{pt.x, pt.y};
    };
    auto two = [this] {   // Bookmarks' folder "two", closed
        HWND tv = tree_.hwnd();
        return TreeView_GetNextSibling(tv, TreeView_GetChild(tv, TreeView_GetRoot(tv)));
    };
    auto dragged = std::make_shared<Com<IDataObject>>();
    add(L"This PC in the tree takes no drop",
        [=, this] {
            HWND tv = tree_.hwnd();
            HTREEITEM pc = TreeView_GetRoot(tv);
            for (HTREEITEM n = pc; n; n = TreeView_GetNextSibling(tv, n)) pc = n;   // the last root
            Com<IDataObject> data = dataObject(wnd_, path(L"file1.txt"));
            dragEffect = DROPEFFECT_COPY;
            treeDrop_->DragEnter(data.get(), MK_LBUTTON, nodePoint(pc), &dragEffect);
            treeDrop_->DragLeave();
        },
        [=, this] { return dragEffect == DROPEFFECT_NONE && !TreeView_GetDropHilight(tree_.hwnd()); });
    add(L"a drag over a folder in the tree lights it up, and the folder takes the file",
        [=, this] {
            CreateDirectoryW(path(L"Bookmarks\\two\\inner").c_str(), nullptr);
            *dragged = dataObject(wnd_, path(L"file1.txt"));
            dragEffect = DROPEFFECT_COPY;
            treeDrop_->DragEnter(dragged->get(), MK_LBUTTON, nodePoint(two()), &dragEffect);
        },
        [=, this] { return dragEffect == DROPEFFECT_COPY && TreeView_GetDropHilight(tree_.hwnd()) == two(); });
    add(L"held over the closed folder, the drag opens it after a moment", nullptr, [=, this] {
        DWORD effect = DROPEFFECT_COPY;   // Windows asks again many times a second while the drag is held there
        treeDrop_->DragOver(MK_LBUTTON, nodePoint(two()), &effect);
        return TreeView_GetChild(tree_.hwnd(), two()) != nullptr;   // "inner" is shown
    });
    add(L"dropping it there copies the file into that folder",
        [=, this] {
            DWORD effect = DROPEFFECT_COPY;
            treeDrop_->Drop(dragged->get(), MK_LBUTTON, nodePoint(two()), &effect);
            dragged->reset();
        },
        [=, this] { return exists(path(L"Bookmarks\\two\\file1.txt")) && !TreeView_GetDropHilight(tree_.hwnd()); },
        10000);
    add(L"a bookmark shortcut to a folder on this PC takes a drop into that folder",
        [=, this] {
            POINTL shortcut = nodePoint(TreeView_GetNextSibling(tree_.hwnd(), two()));   // "sub", to the folder sub
            Com<IDataObject> data = dataObject(wnd_, path(L"renamed.txt"));   // not in sub yet: Windows would ask
            dragEffect = DROPEFFECT_COPY;
            treeDrop_->DragEnter(data.get(), MK_LBUTTON, shortcut, &dragEffect);
            DWORD effect = DROPEFFECT_COPY;
            treeDrop_->Drop(data.get(), MK_LBUTTON, shortcut, &effect);
        },
        [=, this] { return dragEffect == DROPEFFECT_COPY && exists(path(L"sub\\renamed.txt")); }, 10000);
    static double lastTick = 0, longestWait = 0;   // the longest the window went without a timer tick
    add(L"expanding a folder with 10,000 subfolders in the tree keeps the window responsive",   // WinSxS has 20,000
        [=, this] {
            for (int i = 0; i < 10000; ++i)
                CreateDirectoryW(path(L"Bookmarks\\two\\inner\\" + std::to_wstring(i)).c_str(), nullptr);
            TreeView_Expand(tree_.hwnd(), TreeView_GetChild(tree_.hwnd(), two()), TVE_EXPAND);   // "inner"
            lastTick = nowMs();
            longestWait = 0;
        },
        [=, this] {
            longestWait = std::max(longestWait, nowMs() - lastTick);
            lastTick = nowMs();
            HWND tv = tree_.hwnd();
            int shown = 0;
            for (HTREEITEM c = TreeView_GetChild(tv, TreeView_GetChild(tv, two())); c; c = TreeView_GetNextSibling(tv, c))
                ++shown;
            selfTest_->note = std::to_wstring(shown) + L" shown, the window waited up to " +
                              std::to_wstring(int(longestWait)) + L" ms";
            return shown == 10000 && longestWait < 1000;
        },
        10000);
    backHome(L"(back to the test folder)");

    // Saved tabs: Ctrl+S writes a shortcut per tab in Bookmarks\Tabs, Ctrl+O opens their folders as tabs.
    add(L"Ctrl+S saves each tab as a shortcut in Bookmarks\\Tabs, in tab order, replacing the old ones",
        [=, this] {
            CreateDirectoryW(path(L"Bookmarks\\Tabs").c_str(), nullptr);
            makeShortcut(path(L"Bookmarks\\Tabs\\old.lnk"), path(L"sub"));   // an earlier save: replaced
            writeFile(path(L"Bookmarks\\Tabs\\keep.txt"));                   // not a shortcut: kept
            newTab(path(L"sub"), false);
            newTab(L"", false);   // tabs: the test folder, sub, This PC
            selfTestKey('S', true);
        },
        [=, this] {
            Listing saved = listFolder(path(L"Bookmarks\\Tabs"), true);
            sortItems(saved.items, SortKey::Name, true);
            if (saved.items.size() != tabs_.size() + 1 || saved.items.back().name != L"keep.txt") return false;
            for (size_t i = 0; i < tabs_.size(); ++i) {
                bool folder = false;
                auto target = shortcutTarget(path(L"Bookmarks\\Tabs\\" + saved.items[i].name), &folder);
                if (!target || !folder || !samePath(normalizePath(*target), tabs_[i].path)) return false;
            }
            return true;
        });
    add(L"Ctrl+O opens the saved tabs after the open ones, skips any already open, and shows the first new one",
        [=, this] {
            closeTab(2);
            closeTab(1);
            selfTestKey('O', true);
        },
        [=, this] { return tabs_.size() == 3 && samePath(tabs_[1].path, path(L"sub")) && tabs_[2].path.empty() && at(path(L"sub")); });
    add(L"(close those tabs and remove the saved ones)",
        [=, this] {
            closeTab(2);
            closeTab(1);
            deleteTree(path(L"Bookmarks\\Tabs"));
        },
        [=, this] { return tabs_.size() == 1 && at(dir); });

    // Speed: a folder with 10,000 files.
    add(L"loads a 10,000-file folder",
        [=, this] {
            CreateDirectoryW(path(L"big").c_str(), nullptr);
            for (int i = 0; i < 10000; ++i) writeFile(path(L"big\\item " +std::to_wstring(i) + L".dat"));
            t0 = nowMs();
            open(path(L"big"), Nav::New);
        },
        [=, this] {
            if (!at(path(L"big")) || list_.count() != 10000) return false;
            selfTest_->note = L"load " + std::to_wstring(int(nowMs() - t0)) + L" ms";
            return true;
        },
        30000);
    add(L"filters 10,000 files, showing the matches from the top even when scrolled to the bottom",
        [=, this] {
            list_.ensureVisible(9999);
            t0 = nowMs();
            SetWindowTextW(filter_, L"99");
            selfTest_->note = L"filter " + std::to_wstring(int(nowMs() - t0)) + L" ms";
        },
        [=, this] {
            int expected = 0;
            for (int i = 0; i < 10000; ++i) expected += std::to_wstring(i).find(L"99") != std::wstring::npos;
            return list_.count() == expected && list_.topRow() == 0;
        });
    add(L"a run of stars in the filter does not freeze the window (\"********x\" means \"*x\")",
        [=, this] {
            t0 = nowMs();
            SetWindowTextW(filter_, L"********x");   // as when Shift+8 is held a little too long
            keyMs = nowMs() - t0;
            selfTest_->note = L"filter " + std::to_wstring(int(keyMs)) + L" ms";
        },
        [=, this] { return keyMs < 1000 && list_.count() == 0; });
    add(L"sorts 10,000 files by each column",
        [=, this] {
            SetWindowTextW(filter_, L"");
            selfTest_->note = L"sort";
            for (auto [key, name] : {std::pair(SortKey::Name, L"name"), std::pair(SortKey::Type, L"type"),
                                      std::pair(SortKey::Modified, L"date"), std::pair(SortKey::Size, L"size")}) {
                t0 = nowMs();
                list_.setSort(key, false);   // descending: the list is in name order already
                selfTest_->note += std::wstring(L" ") + name + L" " + std::to_wstring(int(nowMs() - t0));
            }
            selfTest_->note += L" ms";
            list_.setSort(SortKey::Name, true);
        },
        [=, this] { return list_.count() == 10000; });
    add(L"a refresh that asks for an item brings it on screen (as after a rename or a new folder)",
        [=, this] {
            list_.ensureVisible(9999);                       // scrolled to the bottom
            refresh(ViewState::selecting(L"item 0.dat"));    // the first row
        },
        [=, this] {
            int row = list_.focused(), first = list_.topRow();   // on screen: within the page that starts at the top row
            selfTest_->note = L"top row " + std::to_wstring(first);
            return idle() && list_.focusedName() == L"item 0.dat" && row >= first &&
                   row < first + ListView_GetCountPerPage(list_.hwnd());
        });
    add(L"Ctrl+T opens a second tab of the folder; the first keeps its selection and scroll position",
        [=, this] {
            SetFocus(list_.hwnd());
            list_.select({L"item 5000.dat"}, L"item 5000.dat");
            list_.scrollToRow(100);   // then scrolled away from it, as with the mouse wheel
            top = list_.topRow();
            selfTestKey('T', true);
        },
        [=, this] { return at(path(L"big")) && tabs_.size() == 2 && active_ == 1; });
    add(L"Ctrl+Tab back restores the selection and the scroll position (4.6)", [=, this] { selfTestKey(VK_TAB, true); },
        [=, this] {
            return idle() && active_ == 0 && list_.focusedName() == L"item 5000.dat" &&
                   list_.selectedNames() == std::vector<std::wstring>{L"item 5000.dat"} && list_.topRow() == top;
        });
    add(L"(select an item in the second tab)", [=, this] { switchTab(1); },
        [=, this] {
            if (!at(path(L"big")) || active_ != 1) return false;
            list_.select({L"item 7000.dat"}, L"item 7000.dat");
            return true;
        });
    add(L"switching away from a tab before it has loaded keeps what both tabs remembered (4.6)",
        [=, this] {
            // Each switch leaves a tab before its folder loads: what it showed before must survive.
            switchTab(0);
            switchTab(1);
            switchTab(0);
        },
        [=, this] {
            return idle() && active_ == 0 && list_.focusedName() == L"item 5000.dat" && list_.topRow() == top;
        },
        10000);   // three loads of 10,000 files: not a false alarm when something else keeps the machine busy
    add(L"(and the second tab keeps its selection too)", [=, this] { switchTab(1); },
        [=, this] { return idle() && active_ == 1 && list_.focusedName() == L"item 7000.dat"; });
    add(L"(back to the first tab)", [=, this] { switchTab(0); }, [=, this] { return idle() && active_ == 0; });
    auto loadsBefore = std::make_shared<std::pair<int, int>>();
    add(L"30 quick Ctrl+Tab presses read the folder to the end once, not 30 times (4.2: a new load replaces the old)",
        [=, this] {
            *loadsBefore = {loadsDone_, loadsWasted_};
            SetFocus(list_.hwnd());
            for (int i = 0; i < 30; ++i) selfTestKey(VK_TAB, true);
        },
        [=, this] {
            int done = loadsDone_ - loadsBefore->first, wasted = loadsWasted_ - loadsBefore->second;
            selfTest_->note =
                std::to_wstring(wasted) + L" of " + std::to_wstring(done) + L" reads ran to the end for nothing";
            return idle() && active_ == 0 && done == 30 && wasted <= 3;
        },
        30000);
    add(L"(filter the first tab down to its selected file, then switch tabs)",
        [=, this] {
            SetFocus(list_.hwnd());
            list_.select({L"item 5000.dat"}, L"item 5000.dat");
            selfTestType(L"5000");
            switchTab(1);
        },
        [=, this] { return idle() && active_ == 1; });
    add(L"coming back to a tab that was filtered shows its selected file (4.6)", [=, this] { switchTab(0); },
        [=, this] {
            int row = list_.focused(), first = list_.topRow();
            selfTest_->note = L"row " + std::to_wstring(row) + L", top row " + std::to_wstring(first);
            return idle() && active_ == 0 && list_.focusedName() == L"item 5000.dat" && row >= first &&
                   row < first + ListView_GetCountPerPage(list_.hwnd());
        });
    add(L"(close the other tab)", [=, this] { closeTab(1); }, [=, this] { return idle() && tabs_.size() == 1; });
    add(L"when another program removes the focused item, the focus moves to the item that took its place (4.5)",
        [=, this] {
            SetFocus(list_.hwnd());
            list_.select({L"item 5000.dat"}, L"item 5000.dat");
            list_.ensureVisible(list_.focused());
            top = list_.topRow();
            DeleteFileW(path(L"big\\item 5000.dat").c_str());
            selfTestKey(VK_F5);   // stands in for the change notification
        },
        [=, this] {
            return idle() && !listed(L"item 5000.dat") && list_.focusedName() == L"item 5001.dat" &&
                   list_.topRow() == top;
        });
    add(L"(put that file back)", [=, this] { writeFile(path(L"big\\item 5000.dat")); refresh(); },
        [=, this] { return idle() && listed(L"item 5000.dat"); });
    add(L"(scroll down, minimize the window, and let another program add a file)",
        [=, this] {
            list_.scrollToRow(5000);
            top = list_.topRow();
            ShowWindow(wnd_, SW_SHOWMINNOACTIVE);
            writeFile(path(L"big\\zz minimized.dat"));   // the folder watch refreshes the list
        },
        [=, this] { return idle() && listed(L"zz minimized.dat"); });
    add(L"a folder that changed while the window was minimized keeps its scroll position (4.5)",
        [=, this] { ShowWindow(wnd_, SW_SHOWNOACTIVATE); },
        [=, this] {
            selfTest_->note = L"top row " + std::to_wstring(list_.topRow()) + L", was " + std::to_wstring(top);
            return list_.topRow() == top;
        });
    add(L"(remove that file)", [=, this] { DeleteFileW(path(L"big\\zz minimized.dat").c_str()); refresh(); },
        [=, this] { return idle() && !listed(L"zz minimized.dat"); });
    // From This PC: from a folder nearby, the old watch would still see the change through a parent's timestamp.
    add(L"(open This PC, so the next folder is not watched yet)", [=, this] { open(L"", Nav::New); }, [=, this] { return at(L""); });
    add(L"a file created while the folder is still being read shows up by itself",
        [=, this] {
            open(path(L"big"), Nav::New);   // 10,000 files: read on a worker thread
            Sleep(100);                     // the read is past the start, where this name sorts
            writeFile(path(L"big\\aaa late.dat"));   // too late for the read, too early for the folder watch
            Sleep(1500);                    // this thread waits: the read ends, and Windows reports the file to no one
        },
        [=, this] { return at(path(L"big")) && listed(L"aaa late.dat"); });
    add(L"(remove that file too)", [=, this] { DeleteFileW(path(L"big\\aaa late.dat").c_str()); refresh(); },
        [=, this] { return idle() && !listed(L"aaa late.dat"); });

    add(L"copies 10,000 selected files to the clipboard",
        [=, this] {
            list_.selectAll();
            t0 = nowMs();
            copyToClipboard(wnd_, list_.dir(), list_.selectedItems(), false);
            selfTest_->note = L"copy " + std::to_wstring(int(nowMs() - t0)) + L" ms";
        },
        [=, this] { return clipboardFiles().size() == 10000; }).copies = true;
    // Files that exist only in the list, all selected: a copy that asked the disk about each one would find nothing.
    auto showGhosts = [=, this] {
        Listing l;
        l.path = path(L"big");
        for (int i = 0; i < 3; ++i) {
            Item it;
            it.name = L"ghost " + std::to_wstring(i) + L".dat";
            it.attrs = FILE_ATTRIBUTE_ARCHIVE;   // as FindFirstFile reports a plain file
            l.items.push_back(it);
        }
        list_.setListing(std::move(l));
        list_.selectAll();
    };
    add(L"copying takes the files from the list, not from the disk",
        [=, this] {
            showGhosts();
            copyToClipboard(wnd_, list_.dir(), list_.selectedItems(), false);
        },
        [=, this] { return clipboardFiles().size() == 3; }).copies = true;
    add(L"a Ctrl+C that cannot reach the clipboard says so, not \"copied\" (another program holds it open)",
        [=, this] {
            // Another thread holds the clipboard open, as a clipboard manager or a remote desktop can.
            HANDLE held = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            std::thread holder([held, release] {
                bool open = OpenClipboard(nullptr);
                SetEvent(held);
                if (open) {
                    WaitForSingleObject(release, 10000);
                    CloseClipboard();
                }
            });
            WaitForSingleObject(held, 5000);
            // Selected again and read at once: a refresh in between would replace the files or clear the message.
            showGhosts();
            setMessage(L"");
            SetFocus(list_.hwnd());
            selfTestKey('C', true);
            selfTest_->note = L"status: " + statusMessage();
            SetEvent(release);
            holder.join();
            CloseHandle(held);
            CloseHandle(release);
        },
        [=, this] {
            return selfTest_->note.find(L"copied") == std::wstring::npos &&
                   selfTest_->note.find(L"clipboard") != std::wstring::npos;
        });

    // Its own timer callback, so the app's code doesn't mention the self-test (qoqdir.exe has none).
    SetTimer(wnd_, kSelfTestTimer, 50, [](HWND w, UINT, UINT_PTR, DWORD) {
        reinterpret_cast<App*>(GetWindowLongPtrW(w, GWLP_USERDATA))->selfTestTick();
    });
}

// One line of the log, written at once (7.2): a hang or a crash later still leaves the steps so far.
void App::selfTestLog(const std::wstring& line) {
    HANDLE f = CreateFileW(selfTest_->logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), int(line.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), int(line.size()), utf8.data(), n, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(f, utf8.data(), DWORD(utf8.size()), &written, nullptr);
    CloseHandle(f);
}

void App::selfTestTick() {
    SelfTest& t = *selfTest_;
    if (t.busy) return;   // an action is running a modal loop (e.g. a progress dialog)
    if (t.index < t.steps.size()) {
        SelfTest::Step& s = t.steps[t.index];
        if (!t.started) {
            if (pending_.generation && !s.whileLoading) return;   // let the previous load finish first
            t.started = true;
            t.note.clear();
            t.busy = true;
            if (s.act) s.act();
            t.busy = false;
            t.startTick = GetTickCount64();   // after the act: its own setup (10,000 files) is not part of the wait
            return;
        }
        bool ok = !s.done || s.done();
        ULONGLONG elapsed = GetTickCount64() - t.startTick;
        if (!ok && elapsed < s.timeoutMs) {
            // The clipboard is shared with every program: the user's own copying may have taken it since. Copy again.
            if (s.copies) s.act();
            return;
        }
        if (!ok) ++t.failures;
        std::wstring line = (ok ? L"ok    " : L"FAIL  ") + s.name + (t.note.empty() ? L"" : L"   [" + t.note + L"]");
        if (elapsed > 500) line += L"   (slow step: " + std::to_wstring(elapsed) + L" ms)";
        selfTestLog(line + L"\n");
        ++t.index;
        t.started = false;
        return;
    }

    KillTimer(wnd_, kSelfTestTimer);
    selfTestLog(t.failures ? std::to_wstring(t.failures) + L" step(s) FAILED\n" : L"all steps passed\n");
    deleteTree(t.dir);
    exitCode_ = t.failures ? 1 : 0;
    PostMessageW(wnd_, WM_CLOSE, 0, 0);
}
