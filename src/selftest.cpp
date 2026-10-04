// "qoqdir.exe --selftest": drives the real app through a scripted session and
// checks the results on disk. Keys are fed straight into App::onKey, so the
// test never sends global keyboard input to other windows.

#include "app.h"

#include <dbt.h>
#include <shellapi.h>

#include <functional>

struct App::SelfTest {
    struct Step {
        std::wstring name;
        std::function<void()> act;
        std::function<bool()> done;
        ULONGLONG timeoutMs = 5000;
        bool whileLoading = false;   // start without waiting for the current folder load to finish
    };
    std::vector<Step> steps;
    size_t index = 0;
    bool started = false, busy = false;
    ULONGLONG startTick = 0;
    std::wstring dir, logPath, note;
    int failures = 0;
};

App::App() = default;
App::~App() = default;

namespace {

std::wstring testRoot() {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
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
        if (it.isDir()) deleteTree(p);
        else DeleteFileW(p.c_str());
    }
    RemoveDirectoryW(dir.c_str());
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::vector<std::wstring> clipboardFiles() {
    std::vector<std::wstring> files;
    if (!OpenClipboard(nullptr)) return files;
    if (HANDLE data = GetClipboardData(CF_HDROP)) {
        auto drop = static_cast<HDROP>(GlobalLock(data));
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t name[MAX_PATH];
            if (DragQueryFileW(drop, i, name, MAX_PATH)) files.push_back(name);
        }
        GlobalUnlock(data);
    }
    CloseClipboard();
    return files;
}

void makeShortcut(const std::wstring& lnk, const std::wstring& target) {
    Com<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void**>(link.put()))))
        return;
    link->SetPath(target.c_str());
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
    // A real Enter/Esc also sends a character, but only while the control is still there to take it.
    if (GetFocus() == focus && (vk == VK_RETURN || vk == VK_ESCAPE)) SendMessageW(focus, WM_CHAR, vk, 0);
}

void App::selfTestType(const std::wstring& text) {
    for (wchar_t c : text)
        if (!onKey({WM_CHAR, WPARAM(c), GetFocus(), false, false, false})) SendMessageW(GetFocus(), WM_CHAR, c, 0);
}

void App::startSelfTest(const std::wstring& dir, const std::wstring& logPath) {
    selfTest_ = std::make_unique<SelfTest>();
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
    auto add = [&t](std::wstring name, std::function<void()> act, std::function<bool()> done, ULONGLONG timeout = 5000,
                    bool whileLoading = false) {
        t.steps.push_back({std::move(name), std::move(act), std::move(done), timeout, whileLoading});
    };
    static double t0 = 0, keyMs = 0;   // keyMs: how long a key press kept the window busy
    static int rows = 0;               // list rows before a refresh that must wait
    static DWORD dragEffect = 0;       // what a simulated drag was allowed to do
    static std::wstring next;          // the item after the one Delete removes
    static int top = 0;                // a scroll position to come back to

    // 6 items: the 5 test files and folders, plus the self-test's own Bookmarks folder.
    add(L"opens the start folder with all 6 items", nullptr, [=, this] { return at(dir) && list_.count() == 6; });
    add(L"shows a Chinese file name", nullptr, [=, this] { return listed(L"\u5831\u544A.pdf"); });
    add(L"typing in the list filters it", [=, this] { SetFocus(list_.hwnd()); selfTestType(L"file"); },
        [=, this] { return list_.count() == 3 && GetFocus() == filter_; });
    add(L"Esc clears the filter and returns to the list", [=, this] { selfTestKey(VK_ESCAPE); },
        [=, this] { return list_.count() == 6 && GetFocus() == list_.hwnd(); });
    // AltGr arrives as Ctrl + Alt: the key must type its character, not fire the Ctrl shortcut.
    add(L"AltGr+L types into the filter instead of focusing the address bar",
        [=, this] {
            selfTestKey('L', true, false, true);                                  // the key down
            onKey({WM_CHAR, WPARAM(L'\u0142'), GetFocus(), true, false, true});   // then the character it makes (l-stroke)
        },
        [=, this] { return GetFocus() == filter_ && GetWindowTextLengthW(filter_) == 1 && list_.count() == 0; });
    add(L"Esc clears that filter too", [=, this] { selfTestKey(VK_ESCAPE); },
        [=, this] { return list_.count() == 6 && GetFocus() == list_.hwnd(); });
    add(L"Ctrl+C puts the file on the clipboard",
        [=, this] { list_.select({L"file1.txt"}, L"file1.txt"); selfTestKey('C', true); },
        [=, this] {
            auto files = clipboardFiles();
            return files.size() == 1 && leafName(files[0]) == L"file1.txt";
        });
    add(L"filter + Enter opens a folder", [=, this] { selfTestType(L"sub"); selfTestKey(VK_RETURN); },
        [=, this] { return at(path(L"sub")); });
    add(L"Ctrl+V pastes with the Windows copy engine", [=, this] { selfTestKey('V', true); },
        [=, this] { return exists(path(L"sub\\file1.txt")); }, 10000);
    add(L"pasted file appears in the list by itself", nullptr, [=, this] { return idle() && listed(L"file1.txt"); });
    add(L"Paste shortcut makes a shortcut to the copied file", [=, this] { pasteInto(wnd_, list_.dir(), true); },
        [=, this] {
            bool folder = false;
            for (const Item& it : listFolder(path(L"sub"), false).items)
                if (isShortcut(it.name) && samePath(shortcutTarget(path(L"sub\\" + it.name), &folder), path(L"file1.txt")))
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
        },
        [=, this] {
            return idle() && !exists(path(L"file10.txt")) && list_.selectedNames() == std::vector<std::wstring>{next};
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
    // The tab's folder is gone but the tab keeps it (4.3): a command must not get it as its working
    // folder, or Windows refuses to start the program (and waits on it first when it is a dead share).
    add(L"(F5 while it is still gone keeps it shown as not available)", [=, this] { selfTestKey(VK_F5); },
        [=, this] {
            return at(path(L"gone")) && list_.count() == 0 && statusMessage().find(L"not available") != std::wstring::npos;
        });
    add(L"a command typed there still runs (the dead folder is not its working folder)",
        [=, this] { SetWindowTextW(address_, L"rundll32"); SetFocus(address_); selfTestKey(VK_RETURN); },
        [=, this] { return at(path(L"gone")) && statusMessage().find(L"not available") != std::wstring::npos; });
    add(L"F5 tries again once it is back", [=, this] { CreateDirectoryW(path(L"gone").c_str(), nullptr); selfTestKey(VK_F5); },
        [=, this] { return at(path(L"gone")) && statusMessage().empty(); });
    add(L"(back up to the test folder)", [=, this] { SetFocus(list_.hwnd()); selfTestKey(VK_BACK); }, [=, this] { return at(dir); });
    add(L"Ctrl+T opens a tab", [=, this] { selfTestKey('T', true); }, [=, this] { return idle() && tabs_.size() == 2; });
    add(L"Ctrl+W closes it", [=, this] { selfTestKey('W', true); }, [=, this] { return idle() && tabs_.size() == 1 && at(dir); });
    add(L"Ctrl+W on the last tab keeps it (the last tab cannot be closed)", [=, this] { selfTestKey('W', true); },
        [=, this] { return idle() && tabs_.size() == 1 && at(dir); });
    add(L"switching tabs shows the new tab at once; nothing acts on the old one meanwhile",
        [=, this] {
            newTab(path(L"sub"), false);
            SetFocus(list_.hwnd());
            switchTab(active_ + 1);
            // Checked right here, before "sub" can have loaded: the address bar already names it, and
            // Ctrl+Shift+N must not create a folder in the tab that is no longer the one shown.
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
    add(L"Enter on a missing typed path shows an error and stays put",
        [=, this] { SetWindowTextW(address_, L"C:\\no\\such\\folder\\anywhere"); SetFocus(address_); selfTestKey(VK_RETURN); },
        [=, this] { return at(dir); });
    add(L"a typed path that fails while a rename box is open leaves the box alone",
        [=, this] {
            // The failure lands after this action (on a dead share it would be a minute later); the user
            // has started a rename meanwhile, which a focus change would commit half-typed.
            SetWindowTextW(address_, L"C:\\no\\such\\folder\\anywhere");
            SetFocus(address_);
            selfTestKey(VK_RETURN);
            list_.select({L"file1.txt"}, L"file1.txt");
            selfTestKey(VK_F2);
        },
        [=, this] { return at(dir) && list_.renaming() && GetFocus() != address_; });
    add(L"(cancel that rename)", [=, this] { ListView_CancelEditLabel(list_.hwnd()); },
        [=, this] { return !list_.renaming(); });
    add(L"a command that does not exist says Can't find",
        [=, this] { SetWindowTextW(address_, L"no-such-program-qoqdir"); SetFocus(address_); selfTestKey(VK_RETURN); },
        [=, this] { return at(dir) && statusMessage().find(L"Can't find") != std::wstring::npos; });

    add(L"typing a folder path opens it",
        [=, this] { SetWindowTextW(address_, path(L"sub").c_str()); SetFocus(address_); selfTestKey(VK_RETURN); },
        [=, this] { return at(path(L"sub")); });
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
    add(L"typing a subfolder's name opens it here (not in Explorer)",
        [=, this] { SetWindowTextW(address_, L"sub"); SetFocus(address_); selfTestKey(VK_RETURN); },
        [=, this] { return at(path(L"sub")); });
    add(L"typing .. goes up", [=, this] { SetWindowTextW(address_, L".."); SetFocus(address_); selfTestKey(VK_RETURN); },
        [=, this] { return at(dir); });
    add(L"typing .\\sub opens it here (not in Explorer)",
        [=, this] { SetWindowTextW(address_, L".\\sub"); SetFocus(address_); selfTestKey(VK_RETURN); },
        [=, this] { return at(path(L"sub")); });
    add(L"typing ..\\ goes up", [=, this] { SetWindowTextW(address_, L"..\\"); SetFocus(address_); selfTestKey(VK_RETURN); },
        [=, this] { return at(dir); });

    // Second review: history entries for deleted folders, and shortcuts.
    add(L"(open a folder that will be deleted)",
        [=, this] { CreateDirectoryW(path(L"temp1").c_str(), nullptr); open(path(L"temp1"), Nav::New); },
        [=, this] { return at(path(L"temp1")); });
    add(L"(go back to the test folder)", [=, this] { open(dir, Nav::New); }, [=, this] { return at(dir); });
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
    add(L"(back up to the test folder)", [=, this] { open(dir, Nav::New); }, [=, this] { return at(dir); });
    add(L"a shortcut whose target ends with a backslash opens the folder with a clean path",
        [=, this] {
            makeShortcut(path(L"slash.lnk"), path(L"sub") + L"\\");   // as hand-made shortcuts often have it
            openShortcut(path(L"slash.lnk"), false);
        },
        [=, this] { return at(path(L"sub")) && tab().path.back() != L'\\'; });   // else the caption and Up break
    add(L"(back up to the test folder)", [=, this] { open(dir, Nav::New); }, [=, this] { return at(dir); });
    add(L"Ctrl+Enter on a shortcut to a folder opens it in a new background tab (not in Explorer)",
        [=, this] {
            list_.select({L"sub-link.lnk"}, L"sub-link.lnk");
            SetFocus(list_.hwnd());
            selfTestKey(VK_RETURN, true);
        },
        [=, this] { return tabs_.size() == 2 && samePath(tabs_[size_t(active_) + 1].path, path(L"sub")) && at(dir); });
    add(L"(close that tab)", [=, this] { closeTab(active_ + 1); }, [=, this] { return idle() && tabs_.size() == 1; });
    add(L"Enter on two shortcuts to folders opens both in background tabs (not in Explorer)",
        [=, this] {
            makeShortcut(path(L"sub-link2.lnk"), path(L"sub"));
            refresh(ViewState{{L"sub-link.lnk", L"sub-link2.lnk"}, L"sub-link.lnk"});
        },
        [=, this] {
            if (!idle() || list_.selection().size() != 2) return false;
            SetFocus(list_.hwnd());
            selfTestKey(VK_RETURN);
            return true;
        });
    add(L"(both folders are in tabs behind the test folder)", nullptr, [=, this] {
        return tabs_.size() == 3 && at(dir) && samePath(tabs_[1].path, path(L"sub")) &&
               samePath(tabs_[2].path, path(L"sub"));
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
            RECT r{};
            TabCtrl_GetItemRect(tabBar_, active_, &r);
            POINT pt{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            ClientToScreen(tabBar_, &pt);
            Com<IDataObject> data = dataObject(wnd_, path(L"file1.txt"));
            POINTL cursor{pt.x, pt.y};
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

    // Regression tests for bugs found in code review.
    add(L"holding F5 while a folder loads does not start more loads",
        [=, this] {
            UINT before = generation_;
            open(path(L"sub"), Nav::New);
            SetFocus(list_.hwnd());
            for (int i = 0; i < 20; ++i) selfTestKey(VK_F5);   // like a held-down key
            selfTest_->note = std::to_wstring(generation_ - before) + L" load(s) started";
        },
        [=, this] { return at(path(L"sub")) && selfTest_->note == L"1 load(s) started"; });
    add(L"(back up to the test folder again)", [=, this] { open(dir, Nav::New); }, [=, this] { return at(dir); });
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
            // Two levels down, no folder watch matches a drive-removed event, so only WM_DEVICECHANGE
            // tells. Stand in for the drive going away: drop the watch, remove the folder, send the message.
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
            newTab(path(L"made"), false);   // a second tab, right of the current one, as from -t or a middle-click
            RECT r{};
            TabCtrl_GetItemRect(tabBar_, active_ + 1, &r);
            POINT pt{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            ClientToScreen(tabBar_, &pt);
            Com<IDataObject> data = dataObject(wnd_, {path(L"file1.txt")});
            POINTL cursor{pt.x, pt.y};
            dragEffect = DROPEFFECT_COPY;
            t0 = nowMs();
            tabDrop_->DragEnter(data.get(), MK_LBUTTON, cursor, &dragEffect);
            tabDrop_->DragOver(MK_LBUTTON, cursor, &dragEffect);
            tabDrop_->DragLeave();
            keyMs = nowMs() - t0;
            selfTest_->note = L"took " + std::to_wstring(int(keyMs)) + L" ms";
        },
        [=, this] { return dragEffect == DROPEFFECT_NONE && keyMs < 100; });
    add(L"(show that tab once)", [=, this] { switchTab(active_ + 1); }, [=, this] { return at(path(L"made")); });
    add(L"(and come back)", [=, this] { switchTab(active_ - 1); }, [=, this] { return at(dir); });
    add(L"dropping a file on a tab puts it in that tab's folder",
        [=, this] {
            RECT r{};
            TabCtrl_GetItemRect(tabBar_, active_ + 1, &r);
            POINT pt{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            ClientToScreen(tabBar_, &pt);
            Com<IDataObject> data = dataObject(wnd_, path(L"file1.txt"));   // what a drag carries
            POINTL cursor{pt.x, pt.y};
            DWORD effect = DROPEFFECT_COPY;
            tabDrop_->DragEnter(data.get(), MK_LBUTTON, cursor, &effect);
            effect = DROPEFFECT_COPY;
            tabDrop_->DragOver(MK_LBUTTON, cursor, &effect);
            effect = DROPEFFECT_COPY;
            tabDrop_->Drop(data.get(), MK_LBUTTON, cursor, &effect);
        },
        [=, this] { return exists(path(L"made\\file1.txt")) && dropTab_ == -1; }, 10000);
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
    auto bookmarkNodes = [this] {   // the texts under Bookmarks, the tree's first root
        std::wstring texts;
        for (HTREEITEM c = TreeView_GetChild(tree_.hwnd(), TreeView_GetRoot(tree_.hwnd())); c; c = TreeView_GetNextSibling(tree_.hwnd(), c)) {
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
    auto rootTexts = [this] {   // the texts of the tree's roots
        std::wstring texts;
        for (HTREEITEM c = TreeView_GetRoot(tree_.hwnd()); c; c = TreeView_GetNextSibling(tree_.hwnd(), c)) {
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
    auto twoRoots = std::make_shared<bool>(false);
    add(L"the user folders arrive from a worker, between Bookmarks and This PC",
        [=, this] {
            tree_.fill();   // as at startup and on a drive change
            // Nothing may ask Windows about the user folders here (4.1): right after fill() the roots are
            // Bookmarks and This PC only; the user folders arrive by message and never before the action ends.
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
        [=] { return bookmarkNodes() == L"one,sub"; });
    add(L"choosing a bookmark shortcut in the tree opens its folder",
        [=, this] {
            TreeView_SelectItem(tree_.hwnd(), TreeView_GetChild(tree_.hwnd(), TreeView_GetRoot(tree_.hwnd())));   // "one"
            SendMessageW(tree_.hwnd(), WM_KEYDOWN, VK_DOWN, 0);   // to "sub", as the user would
        },
        [=, this] { return at(path(L"sub")); });
    add(L"collapsing and expanding a tree folder shows its new subfolders",
        [=, this] {
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
            // After both reads have landed: Bookmarks is back, none of its nodes got the late "deep",
            // and every item the tree counts is reachable (a late insert under a dead handle is counted but lost).
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
    add(L"(back to the test folder)", [=, this] { open(dir, Nav::New); }, [=, this] { return at(dir); });

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
    add(L"filtering while scrolled to the bottom shows the matches from the top",
        [=, this] {
            list_.ensureVisible(9999);
            SetWindowTextW(filter_, L"99");
        },
        [=, this] { return list_.count() > 0 && list_.topRow() == 0; });
    add(L"filters 10,000 files",
        [=, this] {
            SetWindowTextW(filter_, L"");
            t0 = nowMs();
            SetWindowTextW(filter_, L"99");
            selfTest_->note = L"filter " + std::to_wstring(int(nowMs() - t0)) + L" ms";
        },
        [=, this] {
            int expected = 0;
            for (int i = 0; i < 10000; ++i) expected += std::to_wstring(i).find(L"99") != std::wstring::npos;
            return list_.count() == expected;
        });
    add(L"sorts 10,000 files by date",
        [=, this] {
            SetWindowTextW(filter_, L"");
            t0 = nowMs();
            list_.setSort(SortKey::Modified, false);
            selfTest_->note = L"sort " + std::to_wstring(int(nowMs() - t0)) + L" ms";
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
            list_.ensureVisible(list_.focused());
            top = list_.topRow();
            selfTestKey('T', true);
        },
        [=, this] { return at(path(L"big")) && tabs_.size() == 2 && active_ == 1; });
    add(L"Ctrl+Tab back restores the selection and the scroll position (4.6)", [=, this] { selfTestKey(VK_TAB, true); },
        [=, this] {
            return idle() && active_ == 0 && list_.focusedName() == L"item 5000.dat" &&
                   list_.selectedNames() == std::vector<std::wstring>{L"item 5000.dat"} && list_.topRow() == top;
        });
    add(L"(close the other tab)", [=, this] { closeTab(1); }, [=, this] { return idle() && tabs_.size() == 1; });

    add(L"copies 10,000 selected files to the clipboard",
        [=, this] {
            list_.selectAll();
            t0 = nowMs();
            copyToClipboard(wnd_, list_.dir(), list_.selectedItems(), false);
            selfTest_->note = L"copy " + std::to_wstring(int(nowMs() - t0)) + L" ms";
        },
        [=, this] { return clipboardFiles().size() == 10000; });
    add(L"copying takes the files from the list, not from the disk",
        [=, this] {
            // Files that exist only in the list: a copy that asked the disk about each one would find nothing.
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
            copyToClipboard(wnd_, list_.dir(), list_.selectedItems(), false);
        },
        [=, this] { return clipboardFiles().size() == 3; });

    SetTimer(wnd_, kSelfTestTimer, 50, nullptr);
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
            t.startTick = GetTickCount64();
            t.note.clear();
            t.busy = true;
            if (s.act) s.act();
            t.busy = false;
            return;
        }
        bool ok = !s.done || s.done();
        ULONGLONG elapsed = GetTickCount64() - t.startTick;
        if (!ok && elapsed < s.timeoutMs) return;
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
