// "qoqdir-test.exe --selftest": a scripted session in the real app. Keys go straight to App::onKey, never to
// other windows. Only qoqdir-test.exe links this file in; users get qoqdir.exe without it.

#include "app.h"

#include "crashlog.h"

#include <dbt.h>
#include <initguid.h>
#include <oleacc.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <set>
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
    ULONGLONG heldTick = 0;   // since when only a folder load still out has held the next step back
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
        // Read-only is the one attribute that stops DeleteFileW and RemoveDirectoryW.
        if (it.attrs & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!it.isDir()) DeleteFileW(p.c_str());
        else if (it.attrs & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(p.c_str());   // a junction: never followed
        else if (!RemoveDirectoryW(p.c_str())) deleteTree(p);   // an empty folder goes without being listed
    }
    RemoveDirectoryW(dir.c_str());
}

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
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put()))))
        return;
    if (!target.empty()) link->SetPath(target.c_str());
    else if (Pidl pc = pidlFromPath(target)) link->SetIDList(pc.get());
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
    crashLogIn(dir);   // a crash or hang during the test is logged beside its own log
    if (HANDLE f = CreateFileW(logPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        f != INVALID_HANDLE_VALUE)
        CloseHandle(f);   // the log starts empty; every step then appends its line (selfTestLog)
    auto path = [dir](const std::wstring& name) { return joinPath(dir, name); };
    auto idle = [this] { return !loading(); };
    auto at = [this, idle](const std::wstring& p) { return idle() && samePath(tab().path, p); };
    auto statusMessage = [this] { return message(); };
    auto statusHas = [statusMessage](const wchar_t* text) { return statusMessage().find(text) != std::wstring::npos; };
    auto listed = [this](const std::wstring& name) {
        for (int i = 0; i < list_.count(); ++i)
            if (list_.at(i).name == name) return true;
        return false;
    };
    auto editText = [this] {
        HWND edit = ListView_GetEditControl(list_.hwnd());
        return edit ? windowText(edit) : std::wstring();
    };
    auto tabRect = [this](int index) {   // a tab's rectangle in the bar
        RECT r{};
        TabCtrl_GetItemRect(tabBar_, index, &r);
        return r;
    };
    auto tabPoint = [this, tabRect](int index) {   // the middle of a tab, in screen coordinates, where a drag is aimed
        RECT r = tabRect(index);
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
    auto tabWidth = [this, tabRect] {   // the width every tab has (0: they differ)
        int width = 0;
        for (int i = 0; i < TabCtrl_GetItemCount(tabBar_); ++i) {
            RECT r = tabRect(i);
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
    auto volumeChange = [this](WPARAM event) {   // a drive came (DBT_DEVICEARRIVAL) or went, as Windows tells it
        DEV_BROADCAST_VOLUME vol{};
        vol.dbcv_size = sizeof vol;
        vol.dbcv_devicetype = DBT_DEVTYP_VOLUME;
        SendMessageW(wnd_, WM_DEVICECHANGE, event, reinterpret_cast<LPARAM>(&vol));
    };
    auto treeItem = [this](HTREEITEM node, UINT mask) {   // a node's own fields (not its text: that needs a buffer)
        TVITEMW item{};
        item.mask = mask;
        item.hItem = node;
        TreeView_GetItem(tree_.hwnd(), &item);
        return item;
    };
    auto nodeText = [this](HTREEITEM node) {
        wchar_t text[64] = {};
        TVITEMW item{};
        item.mask = TVIF_TEXT;
        item.hItem = node;
        item.pszText = text;
        item.cchTextMax = 64;
        TreeView_GetItem(tree_.hwnd(), &item);
        return std::wstring(text);
    };
    auto expanded = [this](HTREEITEM node) {
        return (TreeView_GetItemState(tree_.hwnd(), node, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
    };
    auto reopenHome = [=, this] { tab().path = dir; open(dir, Nav::Refresh); };   // after a tab was made unavailable
    // A done check that presses Enter on the list once the refresh has selected what `ready` looks for.
    auto enterWhen = [=, this](std::function<bool()> ready) {
        return [=, this] {
            if (!idle() || !ready()) return false;
            SetFocus(list_.hwnd());
            selfTestKey(VK_RETURN);
            return true;
        };
    };
    auto add = [&t](std::wstring name, std::function<void()> act, std::function<bool()> done,
                    ULONGLONG timeout = 5000) -> SelfTest::Step& {
        t.steps.push_back({std::move(name), std::move(act), std::move(done), timeout});
        return t.steps.back();
    };
    auto backHome = [&](const wchar_t* name) {   // a step that opens the test folder again
        add(name, [=, this] { open(dir, Nav::New); }, [=, this] { return at(dir); });
    };
    auto firstTab = [&] {   // a step back to the first tab, on the test folder
        add(L"(back to the first tab)", [=, this] { switchTab(0); }, [=, this] { return at(dir) && active_ == 0; });
    };
    auto oneTabHome = [=, this] { return tabs_.size() == 1 && at(dir); };   // back to the test folder's own tab
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
    // Screen readers read a button's accessible name; a tooltip shows it to everyone else (5.1).
    add(L"the toolbar buttons say what they do and their keys, to screen readers and in tooltips", nullptr, [this] {
        const std::pair<HWND, const wchar_t*> named[] = {{back_, L"Back (Alt+Left)"}, {forward_, L"Forward (Alt+Right)"},
                                                          {up_, L"Up (Backspace)"}, {refresh_, L"Refresh (F5)"}};
        for (auto [h, text] : named) {
            Com<IAccessible> acc;
            if (FAILED(AccessibleObjectFromWindow(h, OBJID_CLIENT, IID_PPV_ARGS(acc.put())))) return false;
            VARIANT self{};
            self.vt = VT_I4;
            self.lVal = CHILDID_SELF;
            BSTR name = nullptr;
            bool ok = SUCCEEDED(acc->get_accName(self, &name)) && name && std::wstring(name) == text;
            SysFreeString(name);
            if (!ok) return false;
        }
        return SendMessageW(tips_, TTM_GETTOOLCOUNT, 0, 0) == 4;
    });
    add(L"typing in the list filters it, and the status bar says so the first time",
        [=, this] { SetFocus(list_.hwnd()); selfTestType(L"file"); },
        [=, this] {
            return list_.count() == 3 && GetFocus() == filter_ &&
                   statusMessage() == L"Typing filters the list; Esc shows all again";
        });
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
            int limit = unscale(std::max(scale(80, dpi_), int(rc.right) - scale(200, dpi_)), dpi_);
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
    add(L"Backspace goes up and selects the folder you came from, and the status bar says so the first time",
        [=, this] { selfTestKey(VK_BACK); },
        [=, this] {
            return at(dir) && list_.focusedName() == L"sub" && list_.selectedNames().size() == 1 &&
                   statusMessage() == L"Up (Alt+Left goes back)";
        });
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
        [=, this] { return at(path(L"gone")) && list_.count() == 0 && statusHas(L"not available"); });
    // The tab keeps its gone folder (4.3), which a command must not get as its working folder.
    add(L"(F5 while it is still gone keeps it shown as not available)", [=, this] { selfTestKey(VK_F5); },
        [=, this] { return at(path(L"gone")) && list_.count() == 0 && statusHas(L"not available"); });
    add(L"a command typed there still runs (the dead folder is not its working folder)",
        [=, this] { goTo(L"rundll32"); },
        [=, this] { return at(path(L"gone")) && statusHas(L"not available"); });
    add(L"F5 tries again once it is back", [=, this] { CreateDirectoryW(path(L"gone").c_str(), nullptr); selfTestKey(VK_F5); },
        [=, this] { return at(path(L"gone")) && list_.shows(path(L"gone")) && !statusHas(L"not available"); });
    add(L"(back up to the test folder)", [=, this] { SetFocus(list_.hwnd()); selfTestKey(VK_BACK); }, [=, this] { return at(dir); });
    add(L"Ctrl+T opens a tab of the same folder", [=, this] { selfTestKey('T', true); },
        [=, this] { return tabs_.size() == 2 && at(dir); });
    add(L"Ctrl+W closes it", [=, this] { selfTestKey('W', true); }, oneTabHome);
    add(L"Ctrl+W on the last tab keeps it (the last tab cannot be closed)", [=, this] { selfTestKey('W', true); },
        oneTabHome);
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
    add(L"(close that tab)", [=, this] { closeTab(1); }, oneTabHome);
    auto dragAlongBar = [this, tabRect](int from, int x) {   // a mouse drag of tab 'from' to x, as the tab bar gets it
        RECT r = tabRect(from);
        int y = (r.top + r.bottom) / 2;
        SendMessageW(tabBar_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM((r.left + r.right) / 2, y));
        SendMessageW(tabBar_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y));
        SendMessageW(tabBar_, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
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
            dragAlongBar(0, tabRect(1).left + 1);   // a slip during a click, short of the next tab's middle
            dragAlongBar(1, tabRect(2).right - 1);
        },
        [=, this] {
            return tabs_.size() == 3 && samePath(tabs_[0].path, dir) && tabs_[1].path.empty() && active_ == 2 &&
                   TabCtrl_GetCurSel(tabBar_) == 2 && at(path(L"sub"));
        });
    add(L"dragging it back to the start puts it first", [=, this] { dragAlongBar(2, tabRect(0).left + 1); },
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
        [=, this] { return active_ == 2 && at(L""); });
    add(L"turning the wheel the other way goes back to the first tab, and stops there",
        [=, this] { for (int i = 0; i < 3; ++i) wheel(120); },
        [=, this] { return active_ == 0 && at(path(L"sub")); });
    add(L"the current tab's name, and only its, starts with a bullet", nullptr, [=, this] {
        for (int i = 0; i < int(tabs_.size()); ++i)
            if (caption(i).starts_with(L"• ") != (i == active_)) return false;
        return true;
    });
    add(L"a new tab goes at the end, so the newest is the last one", [=, this] { selfTestKey('T', true); },
        [=, this] {
            return tabs_.size() == 4 && samePath(tabs_[1].path, dir) && tabs_[2].path.empty() && active_ == 3 &&
                   at(path(L"sub"));
        });
    add(L"(close the other tabs)", [=, this] { closeOtherTabs(1); },   // keeps the test folder's own tab
        oneTabHome);
    add(L"a double-click on a tab opens no tab; one on the empty bar right of the tabs opens one, as \"+\" does",
        [=, this] {
            RECT r = tabRect(0);
            int y = (r.top + r.bottom) / 2;
            for (int x : {(r.left + r.right) / 2, r.right + scale(20, dpi_)}) {   // on the tab, then right of it
                SendMessageW(tabBar_, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(x, y));
                SendMessageW(tabBar_, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
            }
        },
        [=, this] {
            // A real click lands only where the bar says it is its own: right of the last tab.
            RECT r = tabRect(TabCtrl_GetItemCount(tabBar_) - 1);
            POINT pt{r.right + scale(20, dpi_), (r.top + r.bottom) / 2};
            ClientToScreen(tabBar_, &pt);
            bool takesClick = SendMessageW(tabBar_, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y)) == HTCLIENT;
            return takesClick && tabs_.size() == 2 && active_ == 1 && at(dir);
        });
    add(L"(close it)", [=, this] { closeTab(1); }, oneTabHome);
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
            RECT bar{};
            GetClientRect(tabBar_, &bar);
            RECT last = tabRect(int(tabs_.size()) - 1);
            int width = tabWidth();
            selfTest_->note = std::to_wstring(tabs_.size()) + L" tabs " + std::to_wstring(width) + L" wide";
            return width >= scale(80, dpi_) && width < scale(150, dpi_) && last.right <= bar.right;
        });
    add(L"...but no narrower than 80; with more, the bar scrolls", [=, this] { openTabsPast(scale(80, dpi_)); },
        [=, this] {
            HWND arrows = FindWindowExW(tabBar_, nullptr, UPDOWN_CLASSW, nullptr);
            return tabWidth() == scale(80, dpi_) && arrows && IsWindowVisible(arrows);
        });
    add(L"(close them)", [=, this] { closeOtherTabs(active_); }, oneTabHome);
    add(L"a tab the control selects by itself (as assistive tools select one) switches to its folder",
        [=, this] {
            newTab(path(L"sub"), false);        // a second tab, right of this one
            TabCtrl_SetCurFocus(tabBar_, 1);   // on plain tabs this selects, and notifies the window as a click would
        },
        [=, this] { return active_ == 1 && at(path(L"sub")); });
    add(L"(close that tab)", [=, this] { closeTab(1); }, oneTabHome);
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
            selfTest_->note = samePath(windowText(address_), path(L"sub")) ? L"address bar switched" : L"old tab still shown";
            selfTestKey('N', true, true);
        },
        [=, this] {
            return at(path(L"sub")) && selfTest_->note == L"address bar switched" && !exists(path(L"New folder")) &&
                   !exists(path(L"sub\\New folder"));
        });
    // The switch showed where the tab is at once: what is typed into the address bar since is the user's (5.3).
    firstTab();
    add(L"a tab switch whose folder then loads leaves what was typed into the address bar meanwhile (5.3)",
        [=, this] {
            switchTab(1);
            SetWindowTextW(address_, L"typed meanwhile");   // before the tab's folder can have loaded
        },
        [=, this] { return at(path(L"sub")) && windowText(address_) == L"typed meanwhile"; });
    firstTab();
    add(L"...and so does one whose folder turns out not to be available (4.3)",
        [=, this] {
            switchTab(1);
            SetWindowTextW(address_, L"typed meanwhile");
            Listing failed;   // the switch's read, as a share that is off answers it in the end
            failed.path = path(L"sub");
            failed.error = ERROR_BAD_NETPATH;
            onLoaded(std::move(failed), generation_);
        },
        [=, this] {
            return idle() && tab().unavailable && statusHas(L"not available") &&
                   windowText(address_) == L"typed meanwhile";
        });
    add(L"(close that tab)", [=, this] { closeTab(active_); }, oneTabHome);
    add(L"...and so does a move to the folder shown, while the user types there (5.3)",
        [=, this] {
            open(dir, Nav::New);   // as a click on the tree's selected folder: it is read again
            SetFocus(address_);
            SetWindowTextW(address_, L"typed meanwhile");
        },
        [=, this] { return at(dir) && windowText(address_) == L"typed meanwhile"; });
    add(L"(Esc puts the path back and returns to the list)", [=, this] { selfTestKey(VK_ESCAPE); },
        [=, this] { return windowText(address_) == locationText(tab().path) && GetFocus() == list_.hwnd(); });
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
            return at(dir) && statusHas(L"Can't find") && from == 0 && int(to) == GetWindowTextLengthW(address_);
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
            return at(L"") && statusHas(L"Can't find");
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
    add(L"...and opening another folder cancels it",
        [=, this] {
            // The network read reports back after its timeout, during a later step.
            uncountedLoad_ = generation_;
            open(dir, Nav::New);
        },
        [=, this] { return at(dir); }).whileLoading = true;
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
            showUnavailable(dir, ERROR_BAD_NETPATH);   // then it became unreachable
            SendMessageW(wnd_, WM_TIMER, kRefreshTimer, 0);          // and the queued refresh fires
        },
        [=, this] {
            return idle() && tab().unavailable && statusHas(L"not available");
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
        enterWhen([=, this] { return list_.focusedName() == L"sub-link.lnk"; }));
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
        enterWhen([=, this] { return list_.selection().size() == 2; }));
    add(L"(both are in tabs behind the test folder, with clean paths, so with captions)", nullptr, [=, this] {
        if (tabs_.size() != 3 || !at(dir)) return false;
        selfTest_->note = L"tab paths: " + tabs_[1].path + L" | " + tabs_[2].path;
        return samePath(tabs_[1].path, path(L"sub")) && samePath(tabs_[2].path, path(L"sub"));   // not "sub\"
    });
    add(L"(close those tabs)", [=, this] { closeTab(1); closeTab(1); },
        oneTabHome);
    add(L"in a network folder that is not available, paste, new folder and a drag over its tab return at once",
        [=, this] {
            // As if this tab's folder had just failed to load (a real failure can take minutes to arrive).
            tab().path = L"\\\\192.0.2.1\\share";
            showUnavailable(tab().path, ERROR_BAD_NETPATH);
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
    // The windows on the screen (any program's), to see that an action opened none.
    auto visibleWindows = [] {
        std::set<HWND> set;
        EnumWindows([](HWND h, LPARAM p) -> BOOL {
            if (IsWindowVisible(h)) reinterpret_cast<std::set<HWND>*>(p)->insert(h);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&set));
        return set;
    };
    auto windowsBefore = std::make_shared<std::set<HWND>>();
    add(L"Alt+Enter in a tab that is not available opens nothing: there is no folder to show properties of",
        [=, this] {
            *windowsBefore = visibleWindows();
            SetFocus(list_.hwnd());
            selfTestKey(VK_RETURN, false, false, true);
        },
        [=, this] {
            // Still nothing new after a while (the empty list's "" would have opened This PC's System page).
            if (GetTickCount64() - selfTest_->startTick < 1500) return false;
            std::set<HWND> now = visibleWindows();
            return std::includes(windowsBefore->begin(), windowsBefore->end(), now.begin(), now.end());
        });
    add(L"(back to the test folder)", reopenHome, [=, this] { return at(dir); });
    add(L"a shortcut to a folder that is gone says so here (no Windows dialog)",
        [=, this] {
            makeShortcut(path(L"gone.lnk"), path(L"no-such-folder"));
            refresh(ViewState::selecting(L"gone.lnk"));
        },
        enterWhen([=, this] { return list_.focusedName() == L"gone.lnk"; }));
    add(L"(the status bar names the missing folder, and the tab stays put)", nullptr,
        [=, this] { return at(dir) && statusHas(L"no-such-folder"); });
    add(L"a bookmark whose drive letter is gone (a flash drive that came back under another letter) opens its folder "
        L"where the shortcut says it is, relative to itself (5.6)",
        [=, this] {
            // The folder under a drive letter of its own, as on a flash drive: the shortcut is made through the letter,
            // and then the letter goes, as it does when the drive comes back as another one.
            DWORD used = GetLogicalDrives();
            wchar_t letter = L'Z';
            while (letter > L'C' && (used & (1u << (letter - L'A')))) --letter;
            std::wstring drive = std::wstring(1, letter) + L":";
            CreateDirectoryW(path(L"pack").c_str(), nullptr);
            CreateDirectoryW(path(L"pack\\target").c_str(), nullptr);
            DefineDosDeviceW(DDD_NO_BROADCAST_SYSTEM, drive.c_str(), path(L"pack").c_str());
            makeShortcut(drive + L"\\go.lnk", drive + L"\\target");   // both through the letter: Windows stores the way between them
            DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE | DDD_NO_BROADCAST_SYSTEM, drive.c_str(),
                             path(L"pack").c_str());
            openShortcut(path(L"pack\\go.lnk"), false);
        },
        [=, this] { return at(path(L"pack\\target")); });
    backHome(L"(back to the test folder)");
    add(L"(remove them)", [=, this] { deleteTree(path(L"pack")); }, [=, this] { return !exists(path(L"pack")); });
    add(L"a shortcut to This PC opens This PC here (not in Explorer)",
        [=, this] {
            makeShortcut(path(L"pc.lnk"), L"");
            refresh(ViewState::selecting(L"pc.lnk"));
        },
        enterWhen([=, this] { return list_.focusedName() == L"pc.lnk"; }));
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
            volumeChange(DBT_DEVICEREMOVECOMPLETE);
        },
        [=, this] {
            return at(path(L"sub\\deep")) && list_.count() == 0 && statusHas(L"not available");
        });
    add(L"(back to the test folder)", reopenHome, [=, this] { return at(dir); });

    // Slow lookups happen on worker threads (so network drives cannot freeze the window).
    add(L"a program's own icon arrives from the background",
        [=, this] {
            wchar_t cmd[MAX_PATH] = {};
            GetSystemDirectoryW(cmd, MAX_PATH);
            CopyFileW(joinPath(cmd, L"cmd.exe").c_str(), path(L"tool.exe").c_str(), FALSE);
            refresh();
        },
        [=, this] {
            SHFILEINFOW sfi = fileInfo(path(L"tool.exe"), 0, SHGFI_SYSICONINDEX | SHGFI_SMALLICON).value_or(SHFILEINFOW{});
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
        return isDirectory(path(L"Bookmarks")) && nodeText(TreeView_GetRoot(tree_.hwnd())) == L"Bookmarks";
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
    add(L"the tree lists the drives under This PC", nullptr,
        [=, this] { return tree_.lastRoot() && TreeView_GetChild(tree_.hwnd(), tree_.lastRoot()) != nullptr; });
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
            // Done when the subfolders are shown, or the node knows it has none.
            return expected > 0 ? shown == expected : treeItem(root, TVIF_CHILDREN).cChildren == 0;
        });
    auto nodeTexts = [this, nodeText](HTREEITEM first) {   // the texts of a node and its next siblings, comma-separated
        std::wstring texts;
        for (HTREEITEM c = first; c; c = TreeView_GetNextSibling(tree_.hwnd(), c))
            texts += (texts.empty() ? L"" : L",") + nodeText(c);
        return texts;
    };
    auto one = [this] { return TreeView_GetChild(tree_.hwnd(), TreeView_GetRoot(tree_.hwnd())); };   // the first bookmark
    auto bookmarkNodes = [=, this] {   // the texts under Bookmarks, the tree's first root
        return nodeTexts(one());
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
            SHFILEINFOW sfi = fileInfo(knownFolder(FOLDERID_Desktop), 0, SHGFI_DISPLAYNAME).value_or(SHFILEINFOW{});
            std::wstring roots = rootTexts();
            return *twoRoots && roots.starts_with(L"Bookmarks," + std::wstring(sfi.szDisplayName) + L",") &&
                   roots.ends_with(L",This PC");
        });
    add(L"a drive change during the first read of the drives still lists them under This PC",
        [=, this] {
            tree_.fill();   // This PC's read starts
            volumeChange(DBT_DEVICEARRIVAL);   // before it lands
        },
        [=, this] {
            HWND tv = tree_.hwnd();
            return TreeView_GetChild(tv, tree_.lastRoot()) && expanded(tree_.lastRoot());
        });
    auto arrow = std::make_shared<bool>(false);
    add(L"an empty Bookmarks node keeps its arrow, and F5 lists a bookmark added later",
        [=, this] {
            *arrow = treeItem(TreeView_GetRoot(tree_.hwnd()), TVIF_CHILDREN).cChildren == 1;   // the startup read found Bookmarks empty
            CreateDirectoryW(path(L"Bookmarks\\one").c_str(), nullptr);
            selfTestKey(VK_F5);
        },
        [=] { return *arrow && bookmarkNodes() == L"one"; });
    add(L"Bookmarks shows its folders and shortcuts in the tree, open from the start",
        [=, this] {
            CreateDirectoryW(path(L"Bookmarks\\one").c_str(), nullptr);
            makeShortcut(path(L"Bookmarks\\sub.lnk"), path(L"sub"));
            tree_.fill();   // as at startup
        },
        [=, this] {
            if (bookmarkNodes() != L"one,sub") return false;
            // The shortcut's own icon arrives after its node (it may wait on the target).
            SHFILEINFOW own = fileInfo(path(L"Bookmarks\\sub.lnk"), 0, SHGFI_SYSICONINDEX | SHGFI_SMALLICON).value_or(SHFILEINFOW{});
            HTREEITEM sub = TreeView_GetNextSibling(tree_.hwnd(), one());
            return treeItem(sub, TVIF_IMAGE).iImage == own.iIcon;
        });
    add(L"choosing a bookmark shortcut in the tree opens its folder",
        [=, this] {
            TreeView_SelectItem(tree_.hwnd(), one());
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
            TreeView_Expand(tv, one(), TVE_EXPAND);   // its read starts
            TreeView_SelectItem(tv, TreeView_GetRoot(tv));
            SendMessageW(tv, WM_KEYDOWN, VK_LEFT, 0);    // collapse Bookmarks: deletes "one" before its read lands
            SendMessageW(tv, WM_KEYDOWN, VK_RIGHT, 0);   // and read it fresh
        },
        [=, this] {
            // Both reads landed: no late "deep" under Bookmarks, and every node the tree counts is reachable.
            HWND tv = tree_.hwnd();
            if (GetTickCount64() - selfTest_->startTick < 400 || bookmarkNodes() != L"one,two,sub") return false;
            for (HTREEITEM c = one(); c; c = TreeView_GetNextSibling(tv, c))
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
            TreeView_Expand(tree_.hwnd(), one(), TVE_EXPAND);
        },
        [=, this] { return TreeView_GetChild(tree_.hwnd(), one()) != nullptr; });   // "deep" is shown
    add(L"collapsing a folder while F5 reads it again leaves it collapsed, with its arrow (4.5)",
        [=, this] {
            TreeView_SelectItem(tree_.hwnd(), one());   // open, with "deep"
            selfTestKey(VK_F5);                                        // reads every open node again, "one" among them
            SendMessageW(tree_.hwnd(), WM_KEYDOWN, VK_LEFT, 0);       // collapsed before that read lands
        },
        [=, this] {
            HWND tv = tree_.hwnd();
            if (GetTickCount64() - selfTest_->startTick < 400 || !at(dir) || !one()) return false;
            return !expanded(one()) && !TreeView_GetChild(tv, one()) && treeItem(one(), TVIF_CHILDREN).cChildren == 1 &&
                   bookmarkNodes() == L"one,two,sub";
        });
    add(L"(expand it again)",
        [=, this] {   // as the user would: TVM_EXPAND tells nothing about a node expanded before
            TreeView_SelectItem(tree_.hwnd(), one());
            SendMessageW(tree_.hwnd(), WM_KEYDOWN, VK_RIGHT, 0);
        },
        [=, this] { return TreeView_GetChild(tree_.hwnd(), one()) != nullptr; });
    add(L"a drive coming or going re-reads This PC and leaves the rest of the tree as it was (4.5)",
        [=, this] { volumeChange(DBT_DEVICEARRIVAL); },
        [=, this] {
            HWND tv = tree_.hwnd();
            return bookmarkNodes() == L"one,two,sub" && one() && TreeView_GetChild(tv, one()) &&
                   expanded(one()) && TreeView_GetChild(tv, tree_.lastRoot());
        });
    add(L"F5 reads the folders open in the tree again: one that is the same stays as it is (\"one\" stays open), one "
        L"that changed shows its new subfolder (5.5)",
        [=, this] {
            CreateDirectoryW(path(L"Bookmarks\\one\\new").c_str(), nullptr);
            selfTestKey(VK_F5);
        },
        [=, this] {
            return bookmarkNodes() == L"one,two,sub" && nodeTexts(TreeView_GetChild(tree_.hwnd(), one())) == L"deep,new" &&
                   expanded(one());
        });
    add(L"(the status bar says Refreshed)", nullptr, [=, this] { return statusMessage() == L"Refreshed"; });
    auto newNode = [=, this] { return TreeView_GetNextSibling(tree_.hwnd(), TreeView_GetChild(tree_.hwnd(), one())); };
    add(L"(open the folder new in the tree: it has no subfolders, so its arrow goes)",
        [=, this] { TreeView_Expand(tree_.hwnd(), newNode(), TVE_EXPAND); },
        [=, this] { return treeItem(newNode(), TVIF_CHILDREN).cChildren == 0; });
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
            SHFILEINFOW own = fileInfo(path(L"Bookmarks\\one\\sub.lnk"), 0, SHGFI_SYSICONINDEX | SHGFI_SMALLICON).value_or(SHFILEINFOW{});
            HTREEITEM sub = TreeView_GetChild(tree_.hwnd(), one());   // the last child
            while (HTREEITEM after = TreeView_GetNextSibling(tree_.hwnd(), sub)) sub = after;
            TVITEMW item = treeItem(sub, TVIF_IMAGE | TVIF_CHILDREN);
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
    auto two = [=, this] { return TreeView_GetNextSibling(tree_.hwnd(), one()); };   // Bookmarks' folder "two", closed
    auto dragged = std::make_shared<Com<IDataObject>>();
    add(L"This PC in the tree takes no drop",
        [=, this] {
            HTREEITEM pc = tree_.lastRoot();
            Com<IDataObject> data = dataObject(wnd_, path(L"file1.txt"));
            dragEffect = DROPEFFECT_COPY;
            treeDrop_->DragEnter(data.get(), MK_LBUTTON, nodePoint(pc), &dragEffect);
            treeDrop_->DragLeave();
        },
        [=, this] {
            return dragEffect == DROPEFFECT_NONE && !TreeView_GetDropHilight(tree_.hwnd()) &&
                   statusMessage() == L"Only a folder on this PC takes a drop";
        });
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
                if (!target || !folder || !samePath(*target, tabs_[i].path)) return false;
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
        oneTabHome);
    add(L"Bookmark this folder on a tab's menu writes a shortcut to the tab's folder in Bookmarks and shows it in the "
        L"tree (5.6)",
        [=, this] { bookmarkTab(active_); },
        [=, this] {
            return exists(path(L"Bookmarks\\" + leafName(dir) + L".lnk")) &&
                   bookmarkNodes().find(leafName(dir)) != std::wstring::npos;
        });
    add(L"(remove it)",
        [=, this] {
            DeleteFileW(path(L"Bookmarks\\" + leafName(dir) + L".lnk").c_str());
            tree_.refreshFolder(tree_.bookmarks());
        },
        [=, this] { return bookmarkNodes() == L"one,two,sub"; });
    add(L"a shortcut another program makes in Bookmarks (or an Alt+drag onto its node) shows in the tree while another "
        L"folder is shown (5.5)",
        [=, this] { makeShortcut(path(L"Bookmarks\\dropped.lnk"), path(L"sub")); },
        [=, this] { return bookmarkNodes() == L"one,two,dropped,sub"; });
    add(L"(and leaves the tree when deleted)",
        [=, this] { DeleteFileW(path(L"Bookmarks\\dropped.lnk").c_str()); },
        [=, this] { return bookmarkNodes() == L"one,two,sub"; });

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
            Sleep(600);                     // this thread waits: the read ends, and Windows reports the file to no one
        },
        [=, this] { return at(path(L"big")) && listed(L"aaa late.dat"); });
    add(L"(remove that file too)", [=, this] { DeleteFileW(path(L"big\\aaa late.dat").c_str()); refresh(); },
        [=, this] { return idle() && !listed(L"aaa late.dat"); });
    // Shown by a tab switch, This PC must get its own watch and the folder shown before must lose its, with a refresh
    // only that watch asked for: a change there would reload This PC for nothing, and This PC's own changes would go
    // unseen (4.5).
    auto tabLoads = std::make_shared<int>(0);   // loads done before the step's act
    add(L"a refresh only the old folder's watch asked for goes with that watch: This PC loads once (4.5)",
        [=, this] {
            *tabLoads = loadsDone_;
            onFolderChanged();   // stands in for a change reported just before the switch
            newTab(L"", true);
        },
        [=, this] {
            selfTest_->note = std::to_wstring(loadsDone_ - *tabLoads) + L" load(s)";
            return at(L"") && !refreshQueued_ && loadsDone_ == *tabLoads + 1 && tabs_.size() == 2 && active_ == 1;
        });
    auto unwatched = std::make_shared<int>(0);   // loads done before the change
    add(L"switching to a This PC tab drops the other tab's watch: a change in that folder reloads nothing (4.5)",
        [=, this] { *unwatched = loadsDone_; writeFile(path(L"big\\zz unwatched.dat")); },
        [=, this] {
            selfTest_->note = std::to_wstring(loadsDone_ - *unwatched) + L" reload(s)";
            // 0.8 s of nothing (the watch's refresh comes at 400 ms).
            return GetTickCount64() - selfTest_->startTick >= 800 && loadsDone_ == *unwatched;
        });
    add(L"(remove that file and close the tab)",
        [=, this] { DeleteFileW(path(L"big\\zz unwatched.dat").c_str()); closeTab(1); },
        [=, this] { return at(path(L"big")) && tabs_.size() == 1 && !listed(L"zz unwatched.dat"); });
    // Any other refresh stays: This PC lists the drives.
    add(L"a drive that comes while This PC is being read gets This PC read again (4.5)",
        [=, this] {
            *tabLoads = loadsDone_;
            onFolderChanged();   // a change the old watch reported first: the drive's refresh is still kept
            newTab(L"", true);
            volumeChange(DBT_DEVICEARRIVAL);   // before it lands
        },
        [=, this] {
            selfTest_->note = std::to_wstring(loadsDone_ - *tabLoads) + L" load(s)";
            return at(L"") && !refreshQueued_ && loadsDone_ == *tabLoads + 2 && tabs_.size() == 2 && active_ == 1;
        });
    add(L"(close that tab)", [=, this] { closeTab(1); }, [=, this] { return at(path(L"big")) && tabs_.size() == 1; });
    // A switch empties the list, so the list can't say which folder the watch is on: the watch says it.
    add(L"a tab switch that another move overtakes still moves the watch to the folder shown (4.5)",
        [=, this] {
            newTab(path(L"big"), true);   // the move below overtakes its read
            open(L"", Nav::New);
        },
        [=, this] { return at(L"") && tabs_.size() == 2 && active_ == 1 && watch_.watches(L""); });
    add(L"(close that tab too)", [=, this] { closeTab(1); },
        [=, this] { return at(path(L"big")) && tabs_.size() == 1; });
    // The old watch also watched the folder above its own, for that folder going: what it saw there still counts.
    add(L"going up keeps a refresh the old watch asked for: it watched the folder above too (4.5)",
        [=, this] {
            *tabLoads = loadsDone_;
            onFolderChanged();   // stands in for a folder next to big going, which big's watch reports
            open(dir, Nav::New);
        },
        [=, this] {
            selfTest_->note = std::to_wstring(loadsDone_ - *tabLoads) + L" load(s)";
            return at(dir) && !refreshQueued_ && loadsDone_ == *tabLoads + 2;
        });
    add(L"(back into big)", [=, this] { open(path(L"big"), Nav::New); }, [=, this] { return at(path(L"big")); });
    // The switch emptied the list at once: a move that takes over and fails must not leave it empty.
    add(L"a tab switch overtaken by a move that fails still shows the tab's folder, and why the move failed (4.2)",
        [=, this] {
            newTab(path(L"sub"), true);   // the move below overtakes its read
            open(path(L"no such folder"), Nav::New);
        },
        [=, this] {
            return idle() && tabs_.size() == 2 && active_ == 1 && samePath(list_.dir(), path(L"sub")) &&
                   watch_.watches(path(L"sub")) && statusHas(L"no such folder");
        });
    add(L"(close that tab)", [=, this] { closeTab(1); }, [=, this] { return at(path(L"big")) && tabs_.size() == 1; });
    // The tab comes back as the switch would have shown it: the scroll exactly, not moved to the focused file.
    add(L"(a second tab on big)", [=, this] { newTab(path(L"big"), true); },
        [=, this] { return at(path(L"big")) && tabs_.size() == 2 && active_ == 1; });
    add(L"(it focuses a file, is scrolled away from it, and is left for the first tab)",
        [=, this] {
            SetFocus(list_.hwnd());
            list_.select({L"item 5000.dat"}, L"item 5000.dat");
            list_.scrollToRow(100);   // as with the mouse wheel
            top = list_.topRow();
            switchTab(0);
        },
        [=, this] { return idle() && active_ == 0; });
    add(L"a tab whose switch a failed move overtakes comes back with its selection and scroll position (4.6)",
        [=, this] {
            switchTab(1);
            open(path(L"no such folder"), Nav::New);
        },
        [=, this] {
            return idle() && active_ == 1 && list_.shows(path(L"big")) && list_.focusedName() == L"item 5000.dat" &&
                   list_.topRow() == top;
        });
    add(L"(close that tab)", [=, this] { closeTab(1); }, [=, this] { return at(path(L"big")) && tabs_.size() == 1; });
    // An emptied list shows no folder, not This PC: a This PC tab left meanwhile keeps what it remembered.
    auto drive = std::make_shared<std::wstring>();
    add(L"(a This PC tab)", [=, this] { newTab(L"", true); },
        [=, this] { return at(L"") && active_ == 1 && list_.count() > 0; });
    add(L"(its last drive selected, remembered as the tab is left and shown again)",
        [=, this] {
            SetFocus(list_.hwnd());
            *drive = list_.at(list_.count() - 1).name;   // not the first row, which takes the focus by itself
            list_.select({*drive}, *drive);
            switchTab(0);
            switchTab(1);
        },
        [=, this] { return at(L"") && active_ == 1 && list_.focusedName() == *drive; });
    add(L"a This PC tab left while its list is emptied keeps its selection (4.6)",
        [=, this] {
            list_.clear();   // as while a tab switch's read is out
            switchTab(0);
            switchTab(1);
        },
        [=, this] { return at(L"") && active_ == 1 && list_.focusedName() == *drive; });
    add(L"(close that tab too)", [=, this] { closeTab(1); },
        [=, this] { return at(path(L"big")) && tabs_.size() == 1; });
    // On a share that is off, a read of the tab's folder waits 20-40 s. A move that overtakes a tab switch lets the
    // switch's read go on, and the tab shows what it finds as soon as it is back: the folder is not read a second time
    // (4.2). A read still out is stood in for by its number, so the test needs no network.
    constexpr UINT kStuckRead = 0xFFFFFFFF;   // no real read gets this number
    auto reads = std::make_shared<UINT>(0);
    auto saidAgain = std::make_shared<bool>(false);   // the move's error, back in the status bar with the folder
    // To the second tab, and at once on to another folder (most often one that is not there), while the switch's read
    // is still out.
    auto overtake = [=, this](const wchar_t* to) {
        switchTab(1);
        open(path(to), Nav::New);
        // The real read comes back as one replaced, and is dropped.
        if (kept_.generation) kept_.generation = kStuckRead;
    };
    // The stuck read comes back: sub, or the error it ran into; `changed`: sub changed while it was read.
    auto readBack = [=, this](DWORD error, bool changed = false) {
        Listing found;
        found.path = path(L"sub");
        found.written = changed ? FILETIME{} : folderWritten(found.path);
        found.error = error;
        onLoaded(std::move(found), kStuckRead);
    };
    auto moveDone = [this] { return !pending_.generation; };   // the move failed: only the switch's read is out
    // The first tab, which the steps switch away from and back to, shows the test folder: quick to read each time.
    backHome(L"(the first tab on the test folder)");
    add(L"(a second tab on sub)", [=, this] { newTab(path(L"sub"), true); },
        [=, this] { return at(path(L"sub")) && tabs_.size() == 2 && active_ == 1; });
    firstTab();
    add(L"a failed move after a tab switch reads the tab's folder no second time: the switch's read is shown (4.2)",
        [=, this] {
            *reads = generation_;
            switchTab(1);
            open(path(L"missing zero"), Nav::New);
        },
        [=, this] {
            selfTest_->note = std::to_wstring(generation_ - *reads) + L" read(s)";
            return idle() && active_ == 1 && list_.shows(path(L"sub")) && statusHas(L"missing zero") &&
                   generation_ == *reads + 2;
        });
    firstTab();
    add(L"a switch's read that is back while the move is still out shows the tab's folder at once (4.2)",
        [=, this] {
            *reads = generation_;
            overtake(L"missing one");
            readBack(0);
            selfTest_->note = list_.shows(path(L"sub")) && loading() ? L"shown at once" : L"held back";
        },
        [=, this] {
            return selfTest_->note == L"shown at once" && idle() && active_ == 1 && list_.shows(path(L"sub")) &&
                   statusHas(L"missing one") && generation_ == *reads + 2;
        });
    firstTab();
    add(L"a failed move while the switch's read is still out leaves the tab waiting for it, reading no more (4.2)",
        [=, this] {
            *reads = generation_;
            overtake(L"missing two");
        },
        [=, this] {
            selfTest_->note = std::to_wstring(generation_ - *reads) + L" read(s)";
            return loading() && moveDone() && statusHas(L"Loading") && !list_.shows(path(L"sub")) &&
                   generation_ == *reads + 2;
        });
    add(L"...and a refresh asked for meanwhile waits for it, reading nothing beside it (4.2)",
        [=, this] {
            *reads = generation_;
            queueRefresh();
        },
        [=, this] {
            selfTest_->note = std::to_wstring(generation_ - *reads) + L" read(s)";
            // 0.8 s of nothing (the refresh tries every 400 ms).
            return GetTickCount64() - selfTest_->startTick >= 800 && refreshQueued_ && generation_ == *reads &&
                   statusHas(L"Loading");
        }).whileLoading = true;
    add(L"...and once that read is back, the tab shows it with the move's error, then the refresh reads it (4.2)",
        [=, this] {
            SetWindowTextW(address_, L"missing tw");   // as if the user were fixing what they typed (5.3)
            readBack(0);
            *saidAgain = statusHas(L"missing two");   // now: a slow refresh's "Loading..." may take its place
        },
        [=, this] {
            selfTest_->note = std::to_wstring(generation_ - *reads) + L" read(s)";
            return *saidAgain && idle() && list_.shows(path(L"sub")) && !refreshQueued_ && generation_ == *reads + 1 &&
                   windowText(address_) == L"missing tw";
        }).whileLoading = true;
    // What the status bar says after the error stays: "Loading..." gives back what it took the place of.
    firstTab();
    add(L"(a failed move overtakes the switch while its read is still out)", [=, this] { overtake(L"missing three"); },
        [=, this] { return loading() && moveDone() && statusHas(L"Loading"); });
    add(L"a message shown while the tab waits for its switch's read stays once the folder is back (4.2)",
        [=, this] {
            setMessage(L"a newer message");
            readBack(0);
        },
        [=, this] { return idle() && list_.shows(path(L"sub")) && statusHas(L"a newer message"); })
        .whileLoading = true;
    firstTab();
    add(L"(a failed move overtakes the switch while its read is still out, again)",
        [=, this] { overtake(L"missing four"); }, [=, this] { return loading() && moveDone(); });
    add(L"...and a message \"Loading...\" took the place of while it waited comes back with the folder (4.2)",
        [=, this] {
            setMessage(L"a newer message");
            SendMessageW(wnd_, WM_TIMER, kLoadingTimer, 0);   // as if the read took 250 ms more
            readBack(0);
        },
        [=, this] { return idle() && list_.shows(path(L"sub")) && statusHas(L"a newer message"); })
        .whileLoading = true;
    // A read that comes back with an error has its answer: the folder is not available (4.3), no second read.
    firstTab();
    add(L"(a failed move overtakes the switch while its read is still out, once more)",
        [=, this] { overtake(L"missing five"); }, [=, this] { return loading() && moveDone(); });
    add(L"...and if that read comes back with an error, the tab is not available at once (4.3)",
        [=, this] {
            *reads = generation_;
            SetWindowTextW(address_, L"missing fiv");   // as if the user were fixing what they typed (5.3)
            readBack(ERROR_BAD_NETPATH);   // what a share that is off answers in the end
        },
        [=, this] {
            selfTest_->note = std::to_wstring(generation_ - *reads) + L" read(s)";
            return !loading() && generation_ == *reads && tab().unavailable && statusHas(L"is not available") &&
                   windowText(address_) == L"missing fiv";
        }).whileLoading = true;
    add(L"(F5 shows it again)", [=, this] { refresh(); }, [=, this] { return idle() && list_.shows(path(L"sub")); });
    // A move that lands after the switch's read was shown goes on from the tab's folder, as from any other.
    firstTab();
    add(L"a move that lands after the switch's read was shown goes on from the tab's folder (4.2)",
        [=, this] {
            overtake(L"big");
            readBack(0);   // the move is still out
        },
        [=, this] {
            const std::vector<std::wstring>& back = tab().back;
            return at(path(L"big")) && active_ == 1 && !back.empty() && samePath(back.back(), path(L"sub"));
        });
    // A change the switch's read saw is about the tab's folder, as one the watch sees: the move's folder is not read
    // again for it (4.5).
    firstTab();
    add(L"...and a change seen by the switch's read is not read again in the move's folder (4.5)",
        [=, this] {
            *reads = generation_;
            overtake(L"big");
            readBack(0, true);
        },
        [=, this] { return at(path(L"big")) && active_ == 1 && !refreshQueued_ && generation_ == *reads + 2; });
    // The move's own hint, said once it lands (5.3), is still to come when the switch's read fails before it.
    firstTab();
    add(L"a hint of the move waits for it when the switch's read fails first (5.3)",
        [=, this] {
            overtake(L"sub");
            lateHint_ = L"the move's own hint";
            readBack(ERROR_BAD_NETPATH);   // the move is still out
        },
        [=, this] { return at(path(L"sub")) && active_ == 1 && statusHas(L"the move's own hint"); });
    add(L"(close that tab)", [=, this] { closeTab(1); }, oneTabHome);
    // A refresh a move replaced goes with the move: if the move fails, the refresh is asked for again (4.5).
    add(L"a refresh that a failed move replaced is asked for again (4.5)",
        [=, this] {
            *reads = generation_;
            refresh();
            open(path(L"missing six"), Nav::New);
        },
        [=, this] { return idle() && !refreshQueued_ && generation_ == *reads + 3; });
    add(L"(back into big)", [=, this] { open(path(L"big"), Nav::New); }, [=, this] { return at(path(L"big")); });

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
    // Another thread holds the clipboard open, as a clipboard manager or a remote desktop can. It waits its turn:
    // other programs read the clipboard after each copy, and the files copied above come from this window, so it
    // tries again until it has the clipboard, while the window goes on answering between steps. It opens it with
    // a window of its own: held with no window, the clipboard also opens for any program that asks with none, and
    // that program's close frees it.
    struct ClipboardHolder {
        std::atomic<HWND> window = nullptr;   // its window, while it holds the clipboard
        std::atomic<int> tries = 0;
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~ClipboardHolder() { CloseHandle(release); }
    };
    auto holder = std::make_shared<ClipboardHolder>();
    add(L"(another thread takes the clipboard and keeps it open)",
        [=] {
            std::thread([holder] {   // its own reference: the thread can outlast the steps
                // A message-only window: never shown, and destroyed with the thread.
                HWND w = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
                if (!w) return;
                for (ULONGLONG end = GetTickCount64() + 4000;; Sleep(10)) {
                    ++holder->tries;
                    if (OpenClipboard(w)) break;
                    if (GetTickCount64() > end) return;
                }
                holder->window = w;
                WaitForSingleObject(holder->release, 10000);
                CloseClipboard();
                holder->window = nullptr;
            }).detach();
        },
        [=, this] {
            selfTest_->note = std::to_wstring(holder->tries.load()) + L" try(s)";
            return holder->window.load() != nullptr;
        });
    add(L"a Ctrl+C that cannot reach the clipboard says so, not \"copied\" (another program holds it open)",
        [=, this] {
            // Selected again and read at once: a refresh in between would replace the files or clear the message.
            showGhosts();
            setMessage(L"");
            SetFocus(list_.hwnd());
            HWND w = holder->window;
            if (w && GetOpenClipboardWindow() == w) {
                selfTestKey('C', true);
                selfTest_->note = L"status: " + statusMessage();
            } else {
                selfTest_->note = L"the test's hold was gone before the key press";   // no status: the check fails
            }
            SetEvent(holder->release);
            while (holder->window) Sleep(1);   // free again for the steps after this one
        },
        [=, this] {
            return selfTest_->note.find(L"copied") == std::wstring::npos &&
                   selfTest_->note.find(L"clipboard") != std::wstring::npos;
        });
    // 5.8: Open in new window starts this exe on the folder, a title bar down and right of this window. A start would
    // show a window, which the self-test never does: it checks what would be started.
    add(L"Open in new window starts Qoqdir on the folder, a title bar down and right of this window (5.8)", nullptr,
        [=, this] {
            NewWindow w = newWindow(dir);
            std::vector<std::wstring> args = commandLineWords(w.commandLine);
            bool started = args.size() == 2 && samePath(args[0], exePath()) && samePath(w.exe, exePath()) &&
                           parseCommandLine({args[1]}).folders == std::vector<std::wstring>{dir};
            RECT r{};
            GetWindowRect(wnd_, &r);
            MONITORINFO mi{};
            mi.cbSize = sizeof mi;
            GetMonitorInfoW(MonitorFromWindow(wnd_, MONITOR_DEFAULTTONEAREST), &mi);
            int dx = w.place.left - r.left, dy = w.place.top - r.top;
            bool moved = (dx > 0 && dx == dy) || (w.place.left == mi.rcWork.left && w.place.top == mi.rcWork.top);
            bool sameSize = w.place.right - w.place.left == r.right - r.left &&
                            w.place.bottom - w.place.top == r.bottom - r.top;
            return started && moved && sameSize && !w.maximized;
        });
    // 4.10: a window that answers nothing for 5 s gets a line in the crash log saying where its thread is. Last: a
    // hung window gets a ghost window from Windows meanwhile, which can take the focus from the steps after it.
    // The test's own long steps (making and removing 10,000 files) leave such lines too: a new one must appear.
    static std::string crashLog;   // as it was before the stall
    auto readCrashLog = [=] {
        std::string text;
        if (FILE* f = _wfopen(joinPath(dir, L"qoqdir-crash.log").c_str(), L"rb")) {
            char buf[4096];
            for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
            fclose(f);
        }
        return text;
    };
    add(L"a window thread stuck for 7 s leaves a line in the crash log naming where it is",
        [=] { crashLog = readCrashLog(); Sleep(7000); },
        [=] {
            std::string added = readCrashLog().substr(crashLog.size());
            return added.find("Not responding") != std::string::npos && added.find("exe+0x") != std::string::npos;
        }, 15000);

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
    std::string utf8 = toUtf8(line);
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
            if (loading() && !s.whileLoading) {   // let the previous load finish first
                // A load that never ends would hold back every step after it: after a minute the run fails instead.
                if (!t.heldTick) t.heldTick = GetTickCount64();
                if (GetTickCount64() - t.heldTick < 60000) return;
                ++t.failures;
                selfTestLog(L"FAIL  " + s.name + L"   (not started: a folder load never ended)\n");
                t.index = t.steps.size();
                return;
            }
            t.heldTick = 0;
            t.started = true;
            t.note.clear();
            t.busy = true;
            if (s.act) s.act();
            t.busy = false;
            t.startTick = GetTickCount64();   // after the act: its own setup (10,000 files) is not part of the wait
        }
        // Checked at once, not on the next tick: a step that is done right away costs no wait.
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
