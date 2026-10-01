// "qoqdir.exe --selftest": drives the real app through a scripted session and
// checks the results on disk. Keys are fed straight into App::onKey, so the
// test never sends global keyboard input to other windows.

#include "app.h"

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
    std::wstring dir, logPath, log, note;
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

    // 6 items: the 5 test files and folders, plus the self-test's own Bookmarks folder.
    add(L"opens the start folder with all 6 items", nullptr, [=, this] { return at(dir) && list_.count() == 6; });
    add(L"shows a Chinese file name", nullptr, [=, this] { return listed(L"\u5831\u544A.pdf"); });
    add(L"typing in the list filters it", [=, this] { SetFocus(list_.hwnd()); selfTestType(L"file"); },
        [=, this] { return list_.count() == 3 && GetFocus() == filter_; });
    add(L"Esc clears the filter and returns to the list", [=, this] { selfTestKey(VK_ESCAPE); },
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
    add(L"Delete moves the file to the Recycle Bin",
        [=, this] { SetWindowTextW(filter_, L""); SetFocus(list_.hwnd()); selfTestType(L"file10"); selfTestKey(VK_DOWN); selfTestKey(VK_DELETE); },
        [=, this] { return idle() && !exists(path(L"file10.txt")); }, 10000);
    add(L"files created by other programs appear by themselves",
        [=, this] { SetWindowTextW(filter_, L""); writeFile(path(L"external.txt")); },
        [=, this] { return idle() && listed(L"external.txt"); });
    add(L"opening a folder", [=, this] { CreateDirectoryW(path(L"gone").c_str(), nullptr); open(path(L"gone"), Nav::New); },
        [=, this] { return at(path(L"gone")); });
    add(L"...that another program deletes stays in its tab, shown as not available",
        [=, this] { RemoveDirectoryW(path(L"gone").c_str()); },
        [=, this] {
            return at(path(L"gone")) && list_.count() == 0 && statusMessage().find(L"not available") != std::wstring::npos;
        });
    add(L"F5 tries again once it is back", [=, this] { CreateDirectoryW(path(L"gone").c_str(), nullptr); selfTestKey(VK_F5); },
        [=, this] { return at(path(L"gone")) && statusMessage().empty(); });
    add(L"(back up to the test folder)", [=, this] { SetFocus(list_.hwnd()); selfTestKey(VK_BACK); }, [=, this] { return at(dir); });
    add(L"Ctrl+T opens a tab", [=, this] { selfTestKey('T', true); }, [=, this] { return idle() && tabs_.size() == 2; });
    add(L"Ctrl+W closes it", [=, this] { selfTestKey('W', true); }, [=, this] { return idle() && tabs_.size() == 1 && at(dir); });
    add(L"Enter on a missing typed path shows an error and stays put",
        [=, this] { SetWindowTextW(address_, L"C:\\no\\such\\folder\\anywhere"); SetFocus(address_); selfTestKey(VK_RETURN); },
        [=, this] { return at(dir); });

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
    add(L"Ctrl+Enter on a shortcut to a folder opens it in a new background tab (not in Explorer)",
        [=, this] {
            list_.select({L"sub-link.lnk"}, L"sub-link.lnk");
            SetFocus(list_.hwnd());
            selfTestKey(VK_RETURN, true);
        },
        [=, this] { return tabs_.size() == 2 && samePath(tabs_[size_t(active_) + 1].path, path(L"sub")) && at(dir); });
    add(L"(close that tab)", [=, this] { closeTab(active_ + 1); }, [=, this] { return idle() && tabs_.size() == 1; });
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
            Com<IDataObject> data = dataObject(wnd_, {path(L"file1.txt")});
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
        return itemMenu(wnd_, {L""}) && itemMenu(wnd_, {L"C:\\"}) && itemMenu(wnd_, {path(L"file1.txt")});
    });
    add(L"dropping a file on a tab puts it in that tab's folder",
        [=, this] {
            newTab(path(L"made"), false);   // a second tab, right of the current one
            RECT r{};
            TabCtrl_GetItemRect(tabBar_, active_ + 1, &r);
            POINT pt{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            ClientToScreen(tabBar_, &pt);
            Com<IDataObject> data = dataObject(wnd_, {path(L"file1.txt")});   // what a drag carries
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

    add(L"copies 10,000 selected files to the clipboard",
        [=, this] {
            list_.selectAll();
            t0 = nowMs();
            copyToClipboard(wnd_, list_.selectedPaths(), false);
            selfTest_->note = L"copy " + std::to_wstring(int(nowMs() - t0)) + L" ms";
        },
        [=, this] { return clipboardFiles().size() == 10000; });

    SetTimer(wnd_, kSelfTestTimer, 50, nullptr);
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
        t.log += (ok ? L"ok    " : L"FAIL  ") + s.name + (t.note.empty() ? L"" : L"   [" + t.note + L"]");
        if (elapsed > 500) t.log += L"   (slow step: " + std::to_wstring(elapsed) + L" ms)";
        t.log += L"\n";
        ++t.index;
        t.started = false;
        return;
    }

    KillTimer(wnd_, kSelfTestTimer);
    t.log += t.failures ? std::to_wstring(t.failures) + L" step(s) FAILED\n" : L"all steps passed\n";
    deleteTree(t.dir);
    if (HANDLE f = CreateFileW(t.logPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        f != INVALID_HANDLE_VALUE) {
        int n = WideCharToMultiByte(CP_UTF8, 0, t.log.c_str(), int(t.log.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(size_t(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, t.log.c_str(), int(t.log.size()), utf8.data(), n, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(f, utf8.data(), DWORD(utf8.size()), &written, nullptr);
        CloseHandle(f);
    }
    exitCode_ = t.failures ? 1 : 0;
    PostMessageW(wnd_, WM_CLOSE, 0, 0);
}
