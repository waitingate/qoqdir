#include "app.h"

#include "shell.h"

#include <dbt.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <optional>
#include <thread>

namespace {

enum : int { ID_BACK = 100, ID_FORWARD, ID_UP, ID_ADDRESS, ID_FILTER, ID_TABS, ID_PLUS, ID_TREE, ID_LIST, ID_STATUS };
enum : UINT {
    WM_APP_LOADED = WM_APP + 1, WM_APP_CHANGED, WM_APP_CLOSETAB, WM_APP_RENAME, WM_APP_ICONS, WM_APP_TREE,
    WM_APP_MIDDLECLICK, WM_APP_TABS_SAVED, WM_APP_TABS_READ
};

constexpr size_t kMaxHistory = 100;

// The newest load, for the load threads: a replaced one stops reading (4.2). Global: a thread can outlive the window.
std::atomic<UINT> g_newestLoad{0};

constexpr COLORREF kBackground = RGB(235, 235, 235);   // soft gray: less glare than white

bool is(const std::wstring& verb, const wchar_t* name) { return _wcsicmp(verb.c_str(), name) == 0; }

std::wstring windowText(HWND h) {
    std::wstring s(size_t(GetWindowTextLengthW(h)) + 1, L'\0');
    s.resize(size_t(GetWindowTextW(h, s.data(), int(s.size()))));
    return s;
}

void focusEdit(HWND edit) {   // with its text selected, ready to be typed over or corrected
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 0, -1);
}

// Ctrl+Backspace as in Explorer: deletes the selection or the word before the cursor (an edit box types a box).
void deleteWordBefore(HWND edit) {
    DWORD start = 0, end = 0;
    SendMessageW(edit, EM_GETSEL, WPARAM(&start), LPARAM(&end));
    std::wstring text = windowText(edit);
    if (start == end) {
        while (start > 0 && iswspace(text[start - 1])) --start;
        while (start > 0 && !iswspace(text[start - 1])) --start;
    }
    SendMessageW(edit, EM_SETSEL, start, end);
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
}

// A shortcut's target that opens in a tab: a folder path, or This PC (""). Anything else is for Windows.
bool opensInTab(const std::optional<std::wstring>& target) {
    return target && (target->empty() || isAbsolutePath(*target));
}

// Middle-click closes a tab.
LRESULT CALLBACK tabBarProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_MBUTTONUP) {
        TCHITTESTINFO hit{};
        hit.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int index = TabCtrl_HitTest(h, &hit);
        if (index >= 0) PostMessageW(GetParent(h), WM_APP_CLOSETAB, WPARAM(index), 0);
        return 0;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// Middle-click in the file list or the tree: tell the main window where (screen coordinates).
LRESULT CALLBACK middleClickProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_MBUTTONUP) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(h, &pt);
        PostMessageW(GetParent(h), WM_APP_MIDDLECLICK, reinterpret_cast<WPARAM>(h), MAKELPARAM(pt.x, pt.y));
        return 0;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

}  // namespace

// ---------------------------------------------------------------- setup

bool App::create(HINSTANCE instance, const CommandLine& cmd, bool useSettings) {
    useSettings_ = useSettings;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(kBackground);   // lives as long as the process
    wc.lpszClassName = kAppName;
    RegisterClassExW(&wc);

    UINT sysDpi = GetDpiForSystem();
    CreateWindowExW(0, wc.lpszClassName, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                    scale(1100, sysDpi), scale(700, sysDpi), nullptr, nullptr, instance, this);
    if (!wnd_) return false;
    dpi_ = GetDpiForWindow(wnd_);

    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exStyle, int id) {
        return CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, wnd_,
                               reinterpret_cast<HMENU>(INT_PTR(id)), instance, nullptr);
    };
    back_ = make(L"BUTTON", L"←", BS_PUSHBUTTON, 0, ID_BACK);
    forward_ = make(L"BUTTON", L"→", BS_PUSHBUTTON, 0, ID_FORWARD);
    up_ = make(L"BUTTON", L"↑", BS_PUSHBUTTON, 0, ID_UP);
    address_ = make(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, ID_ADDRESS);
    filter_ = make(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, ID_FILTER);
    // Flat button-style tabs: Windows outlines the selected one, which reads clearly on gray.
    tabBar_ = make(WC_TABCONTROLW, L"", TCS_FOCUSNEVER | TCS_BUTTONS | TCS_FLATBUTTONS, 0, ID_TABS);
    plus_ = make(L"BUTTON", L"+", BS_PUSHBUTTON, 0, ID_PLUS);
    // Bookmarks are shortcuts in a folder next to the exe (in a self-test: in the test folder).
    std::wstring exe(32768, L'\0');   // not MAX_PATH: the app may live in a deep folder
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size())));
    std::wstring bookmarks = joinPath(useSettings ? parentPath(exe) : cmd.folders.at(0), L"Bookmarks");
    CreateDirectoryW(bookmarks.c_str(), nullptr);   // fails harmlessly if it exists
    tree_.create(wnd_, ID_TREE, WM_APP_TREE, WM_APP_ICONS, bookmarks);
    list_.create(wnd_, ID_LIST);
    status_ = make(STATUSCLASSNAMEW, L"", SBARS_SIZEGRIP, 0, ID_STATUS);

    SendMessageW(filter_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Filter  (just start typing)"));
    SHAutoComplete(address_, SHACF_FILESYS_DIRS);
    SetWindowSubclass(tabBar_, tabBarProc, 1, 0);
    SetWindowSubclass(list_.hwnd(), middleClickProc, 1, 0);
    SetWindowSubclass(tree_.hwnd(), middleClickProc, 1, 0);
    // Drops land in the current folder; never in This PC, and never back onto our own drag.
    IDropTarget* drop = newDropTarget(wnd_, [this](const POINT* pt) -> std::optional<std::wstring> {
        if (!pt || draggingFiles_ || list_.dir().empty()) return std::nullopt;
        return list_.dir();
    });
    RegisterDragDrop(list_.hwnd(), drop);
    drop->Release();
    // Dropping on a tab puts the files in that tab's folder.
    *tabDrop_.put() = newDropTarget(wnd_, [this](const POINT* pt) { return tabDropTarget(pt); });
    RegisterDragDrop(tabBar_, tabDrop_.get());

    ListView_SetBkColor(list_.hwnd(), kBackground);
    ListView_SetTextBkColor(list_.hwnd(), kBackground);
    TreeView_SetBkColor(tree_.hwnd(), kBackground);
    applyFont();
    tree_.fill();

    // Without settings (self-test), read from a file that does not exist: every value is a default.
    ini_ = useSettings_ ? joinPath(parentPath(exe), toLower(kAppName) + L".ini") : L"?";
    loadSettings(cmd);
    layout();
    showTabs();
    open(tab().path, Nav::Switch, tab().view);
    focusList();
    return true;
}

LRESULT CALLBACK App::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        app->wnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return app ? app->onMessage(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

void App::applyFont() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    Font font(CreateFontIndirectW(&ncm.lfMessageFont));
    for (HWND h : {back_, forward_, up_, address_, filter_, tabBar_, plus_, tree_.hwnd(), list_.hwnd(), status_})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font.get()), TRUE);
    font_ = std::move(font);   // the old font is deleted only after no control uses it

    HDC dc = GetDC(wnd_);
    HGDIOBJ old = SelectObject(dc, font_.get());
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(wnd_, dc);
    rowHeight_ = tm.tmHeight + scale(10, dpi_);
    tabHeight_ = tm.tmHeight + scale(12, dpi_);   // tab text plus the control's own padding
}

void App::layout() {
    if (!status_) return;   // still creating the child windows
    RECT rc;
    GetClientRect(wnd_, &rc);
    int w = rc.right, h = rc.bottom;
    SendMessageW(status_, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(status_, &sr);
    int statusH = sr.bottom - sr.top;
    int parts[2] = {scale(380, dpi_), -1};
    SendMessageW(status_, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));

    int pad = scale(4, dpi_), btn = std::max(rowHeight_, scale(30, dpi_)), gap = scale(2, dpi_);
    int filterW = std::min(scale(240, dpi_), w / 3);
    int y = pad, x = pad;
    HDWP d = BeginDeferWindowPos(10);
    auto place = [&](HWND hwnd, int px, int py, int pw, int ph) {
        if (d) d = DeferWindowPos(d, hwnd, nullptr, px, py, std::max(pw, 0), std::max(ph, 0), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    for (HWND b : {back_, forward_, up_}) {
        place(b, x, y, btn, rowHeight_);
        x += btn + gap;
    }
    x += pad;
    place(address_, x, y, w - x - pad - filterW - pad, rowHeight_);
    place(filter_, w - pad - filterW, y, filterW, rowHeight_);
    y += rowHeight_ + pad;
    place(tabBar_, 0, y, w - btn - pad, tabHeight_);
    place(plus_, w - btn - pad, y + (tabHeight_ - rowHeight_) / 2, btn, rowHeight_);
    y += tabHeight_;

    int splitW = scale(kSplitterWidth, dpi_);
    int treeW = std::clamp(scale(treeWidth_, dpi_), scale(80, dpi_), std::max(scale(80, dpi_), w - scale(200, dpi_)));
    int bodyH = h - statusH - y;
    place(tree_.hwnd(), 0, y, treeW, bodyH);
    place(list_.hwnd(), treeW + splitW, y, w - treeW - splitW, bodyH);
    splitterX_ = treeW;
    if (d) EndDeferWindowPos(d);
}

// ---------------------------------------------------------------- messages

LRESULT App::onMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:   // not while minimized: a list with no height loses its scroll position in a refresh (4.5)
        if (wp != SIZE_MINIMIZED) layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {scale(520, dpi_), scale(320, dpi_)};
        return 0;
    }
    case WM_DPICHANGED: {
        UINT newDpi = HIWORD(wp);
        std::vector<int> widths = list_.columnWidths();
        for (int& cw : widths) cw = MulDiv(cw, int(newDpi), int(dpi_));
        dpi_ = newDpi;
        applyFont();
        list_.setColumnWidths(widths);
        const RECT* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(wnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();   // the size may not have changed, but the font and row heights did
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            lastFocus_ = GetFocus();
        } else if (lastFocus_ && IsChild(wnd_, lastFocus_)) {
            SetFocus(lastFocus_);
        } else {
            SetFocus(list_.hwnd());
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == ID_FILTER && code == EN_CHANGE) {
            list_.setFilter(windowText(filter_));
            return 0;
        }
        if (code != BN_CLICKED) return 0;
        if (id == ID_BACK) goBack();
        if (id == ID_FORWARD) goForward();
        if (id == ID_UP) goUp();
        if (id == ID_PLUS) newTab(tab().path, true);
        focusList();
        return 0;
    }
    case WM_NOTIFY:
        return onNotify(reinterpret_cast<NMHDR*>(lp));
    case WM_CONTEXTMENU:
        onContextMenu(reinterpret_cast<HWND>(wp), {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        return 0;
    case WM_INITMENUPOPUP:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_MENUCHAR: {
        LRESULT r = 0;
        if (forwardMenuMessage(msg, wp, lp, &r)) return r;
        break;
    }
    case WM_APP_LOADED: {
        std::unique_ptr<Listing> listing(reinterpret_cast<Listing*>(lp));
        onLoaded(std::move(*listing), UINT(wp));
        return 0;
    }
    case WM_APP_CHANGED:
        acknowledgeChange(wp, lp);
        queueRefresh();
        return 0;
    case WM_APP_ICONS: {
        std::unique_ptr<IconBatch> batch(reinterpret_cast<IconBatch*>(lp));
        rememberIcons(*batch);
        if (samePath(list_.dir(), batch->dir)) InvalidateRect(list_.hwnd(), nullptr, FALSE);
        tree_.setIcons(*batch);   // bookmark shortcuts get their own icons the same way
        return 0;
    }
    case WM_APP_TREE: {
        std::unique_ptr<TreeBatch> batch(reinterpret_cast<TreeBatch*>(lp));
        tree_.addChildren(*batch);
        return 0;
    }
    case WM_APP_MIDDLECLICK:
        onMiddleClick(reinterpret_cast<HWND>(wp), {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        return 0;
    case WM_APP_TABS_SAVED:
        setMessage(wp ? L"Tabs saved in Bookmarks\\Tabs" : L"Can't save every tab in Bookmarks\\Tabs");
        if (!wp) warn();
        return 0;
    case WM_APP_TABS_READ: {
        std::unique_ptr<std::vector<std::wstring>> folders(reinterpret_cast<std::vector<std::wstring>*>(lp));
        addSavedTabs(*folders);
        return 0;
    }
    case WM_APP_CLOSETAB:
        closeTab(int(wp));
        return 0;
    case WM_APP_RENAME: {
        Rename r = std::exchange(rename_, {});
        if (!r.path.empty() && SUCCEEDED(renameItem(wnd_, r.path, r.name))) {
            refresh(ViewState::selecting(r.name));
        }
        return 0;
    }
    case WM_TIMER:
        onTimer(wp);
        return 0;
    case WM_APPCOMMAND:
        switch (GET_APPCOMMAND_LPARAM(lp)) {
        case APPCOMMAND_BROWSER_BACKWARD: goBack(); return TRUE;
        case APPCOMMAND_BROWSER_FORWARD: goForward(); return TRUE;
        case APPCOMMAND_BROWSER_REFRESH: refresh(); return TRUE;
        }
        break;
    case WM_DEVICECHANGE:
        if ((wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) && lp &&
            reinterpret_cast<DEV_BROADCAST_HDR*>(lp)->dbch_devicetype == DBT_DEVTYP_VOLUME) {
            tree_.refreshDrives();   // only This PC changes; the rest of the tree stays as the user left it
            queueRefresh();   // This PC lists drives, and a tab's folder may be on the drive (its watch can't tell)
        }
        return TRUE;
    case WM_SETCURSOR:
        if (reinterpret_cast<HWND>(wp) == wnd_ && LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(wnd_, &p);
            if (overSplitter(p.x)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp);
        if (overSplitter(x)) {
            dragging_ = true;
            SetCapture(wnd_);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (dragging_) {
            treeWidth_ = MulDiv(std::max(GET_X_LPARAM(lp), 0), 96, int(dpi_));
            layout();
        }
        return 0;
    case WM_LBUTTONUP:
        if (dragging_) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        dragging_ = false;
        return 0;
    case WM_ENDSESSION:
        // Shutdown or sign-out ends the process without a WM_CLOSE: save here too (5.11).
        if (wp) saveSettings();
        return 0;
    case WM_CLOSE:
        saveSettings();
        DestroyWindow(wnd_);
        return 0;
    case WM_DESTROY:
        watch_ = {};
        RevokeDragDrop(list_.hwnd());
        RevokeDragDrop(tabBar_);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd_, msg, wp, lp);
}

// At most one refresh per interval; it waits while a rename or a load is in progress (onTimer).
void App::queueRefresh() {
    if (refreshQueued_) return;
    refreshQueued_ = true;
    SetTimer(wnd_, kRefreshTimer, 400, nullptr);
}

void App::onTimer(UINT_PTR id) {
    if (id == kRefreshTimer) {
        // Try again later: never disturb a rename, and never cancel a folder that is loading.
        if (list_.renaming() || pending_.generation) return;
        KillTimer(wnd_, kRefreshTimer);
        refreshQueued_ = false;
        // What a delete, rename or new folder asked to select, if its folder is still shown. Not available: F5 (4.3).
        QueuedView q = std::exchange(queuedView_, {});
        if (!tab().unavailable) refresh(samePath(list_.dir(), q.dir) ? std::move(q.view) : ViewState{});
    } else if (id == kStatusTimer) {
        KillTimer(wnd_, kStatusTimer);
        showStatus();
    } else if (id == kLoadingTimer) {
        KillTimer(wnd_, kLoadingTimer);
        if (pending_.generation) {   // say "Loading..." only if it is slow
            setMessage(L"Loading…");
            loadMessageShown_ = true;
        }
    }
}

// Middle-click on a folder in the list or the tree opens it in a new tab, in the background.
void App::onMiddleClick(HWND from, POINT screenPt) {
    std::wstring path;
    if (from == list_.hwnd()) {
        LVHITTESTINFO hit{};
        hit.pt = screenPt;
        ScreenToClient(from, &hit.pt);
        int row = ListView_HitTest(from, &hit);
        if (row >= 0 && row < list_.count() && list_.at(row).isDir()) newTab(list_.pathAt(row), false);
    } else if (from == tree_.hwnd() && tree_.pathAt(screenPt, &path)) {
        openNode(path, true);
    }
}

LRESULT App::onNotify(NMHDR* nm) {
    if (nm->hwndFrom == list_.hwnd()) {
        LRESULT result = 0;
        if (list_.onNotify(nm, &result)) return result;
        switch (nm->code) {
        case LVN_ITEMCHANGED:
        case LVN_ODSTATECHANGED:
            SetTimer(wnd_, kStatusTimer, 30, nullptr);   // once per burst, not once per item
            break;
        case LVN_ITEMACTIVATE:
            activate(GetKeyState(VK_CONTROL) < 0);
            break;
        case LVN_ENDLABELEDITW: {
            auto* info = reinterpret_cast<NMLVDISPINFOW*>(nm);
            int row = info->item.iItem;
            if (info->item.pszText && info->item.pszText[0] && row >= 0 && row < list_.count() && info->item.pszText != list_.at(row).name) {
                rename_ = {list_.pathAt(row), info->item.pszText};
                PostMessageW(wnd_, WM_APP_RENAME, 0, 0);
            }
            return FALSE;
        }
        case LVN_BEGINDRAG:
        case LVN_BEGINRDRAG: {
            std::vector<Item> items = list_.selectedItems();
            if (!items.empty()) {
                draggingFiles_ = true;
                dragOut(wnd_, list_.dir(), items);
                draggingFiles_ = false;
            }
            break;
        }
        }
        return 0;
    }
    if (nm->hwndFrom == tree_.hwnd()) {
        std::wstring path;
        if (tree_.onNotify(nm, &path)) openNode(path, false);
        return 0;
    }
    if (nm->hwndFrom == tabBar_) {
        if (nm->code == TCN_SELCHANGE) switchTab(TabCtrl_GetCurSel(tabBar_));
        if (nm->code == NM_RCLICK) onTabMenu();
        return 0;
    }
    return 0;
}

void App::onContextMenu(HWND from, POINT pt) {
    bool keyboard = pt.x == -1 && pt.y == -1;
    if (from == list_.hwnd()) {
        if (list_.renaming()) return;
        if (keyboard) {
            RECT r{};
            int row = list_.focused();
            if (row >= 0 && ListView_GetItemRect(list_.hwnd(), row, &r, LVIR_LABEL)) pt = {r.left, r.bottom};
            else pt = {0, 0};
            ClientToScreen(list_.hwnd(), &pt);
        }
        std::vector<int> rows = list_.selection();
        // The menu is modal and a load can land meanwhile: then the app's own commands do nothing.
        const std::wstring dir = list_.dir();
        const std::vector<std::wstring> names = list_.selectedNames();
        if (rows.empty()) {
            if (!dir.empty())
                showMenu(wnd_, backgroundMenu(wnd_, dir).get(), pt, nullptr,
                         [&](bool shortcuts) { if (samePath(list_.dir(), dir)) pasteInto(wnd_, dir, shortcuts); });
            return;
        }
        // With a folder or shortcut among them, activate opens them all (folders here, files with Windows).
        bool opensHere = std::any_of(rows.begin(), rows.end(), [&](int r) {
            return list_.at(r).isDir() || isShortcut(list_.at(r).name);
        });
        showMenu(wnd_, itemMenu(wnd_, dir, list_.selectedItems()).get(), pt, [&](const std::wstring& verb) {
            bool open = is(verb, L"open") || is(verb, L"explore");
            bool inNewTab = is(verb, L"opennewtab") || is(verb, L"opennewwindow");
            if (!is(verb, L"rename") && !is(verb, L"delete") && !(opensHere && (open || inNewTab))) return false;
            if (!samePath(list_.dir(), dir) || list_.selectedNames() != names) return true;   // changed: do nothing
            if (is(verb, L"rename")) list_.rename(list_.focused() >= 0 ? list_.focused() : rows[0]);
            else if (is(verb, L"delete")) deleteSelection(GetKeyState(VK_SHIFT) < 0);   // like the Delete key (4.6)
            else activate(inNewTab);
            return true;
        });
        return;
    }
    if (from == tree_.hwnd()) {
        std::wstring path;
        if (!tree_.pathAt(pt, &path)) return;
        if (keyboard) {
            RECT r{};
            GetWindowRect(tree_.hwnd(), &r);
            pt = {r.left + scale(24, dpi_), r.top + scale(24, dpi_)};
        }
        showMenu(wnd_, itemMenu(wnd_, path).get(), pt, [&](const std::wstring& verb) {
            if (is(verb, L"open") || is(verb, L"explore")) openNode(path, false);
            else if (is(verb, L"opennewtab") || is(verb, L"opennewwindow")) openNode(path, true);
            else if (is(verb, L"rename")) renameNode(path);
            else return false;
            return true;
        });
    }
}

void App::onTabMenu() {
    TCHITTESTINFO hit{};
    GetCursorPos(&hit.pt);
    POINT screen = hit.pt;
    ScreenToClient(tabBar_, &hit.pt);
    int index = TabCtrl_HitTest(tabBar_, &hit);
    if (index < 0) return;
    Menu menu(CreatePopupMenu());
    AppendMenuW(menu.get(), MF_STRING, 1, L"Duplicate tab");
    AppendMenuW(menu.get(), MF_STRING, 2, L"Close tab\tCtrl+W");
    AppendMenuW(menu.get(), MF_STRING | (tabs_.size() > 1 ? 0 : MF_GRAYED), 3, L"Close other tabs");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING | (index > 0 ? 0 : MF_GRAYED), 4, L"Move left\tCtrl+Shift+PgUp");
    AppendMenuW(menu.get(), MF_STRING | (index + 1 < int(tabs_.size()) ? 0 : MF_GRAYED), 5, L"Move right\tCtrl+Shift+PgDn");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, 6, L"Save tabs\tCtrl+S");
    AppendMenuW(menu.get(), MF_STRING, 7, L"Open saved tabs\tCtrl+O");
    int cmd = TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, wnd_, nullptr);
    if (cmd == 1) newTab(tabs_[size_t(index)].path, true);
    if (cmd == 2) closeTab(index);
    if (cmd == 3) closeOtherTabs(index);
    if (cmd == 4 || cmd == 5) moveTab(index, cmd == 4 ? -1 : 1);
    if (cmd == 6) saveTabs();
    if (cmd == 7) openSavedTabs();
}

// ---------------------------------------------------------------- keyboard

bool App::handleKey(const MSG& m) {
    if (m.message != WM_KEYDOWN && m.message != WM_SYSKEYDOWN && m.message != WM_CHAR) return false;
    // Only our own window: never steal keys from dialogs such as Properties.
    if (!wnd_ || (m.hwnd != wnd_ && !IsChild(wnd_, m.hwnd))) return false;
    return onKey({m.message, m.wParam, GetFocus(), GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0,
                  GetKeyState(VK_MENU) < 0});
}

bool App::onKey(const Key& key) {
    if (list_.renaming()) return false;   // the rename box handles its own keys
    HWND focus = key.focus, list = list_.hwnd();
    bool ctrl = key.ctrl, shift = key.shift, alt = key.alt;
    if (ctrl && alt) ctrl = alt = false;   // AltGr reads as Ctrl+Alt and types a character; no shortcut uses both

    if (key.message == WM_CHAR) {
        // Typing in the file list filters it.
        if (focus == list && key.code >= 32 && key.code != 127 && !ctrl && !alt) {
            SetFocus(filter_);
            int len = GetWindowTextLengthW(filter_);
            SendMessageW(filter_, EM_SETSEL, WPARAM(len), LPARAM(len));
            SendMessageW(filter_, WM_CHAR, key.code, 0);
            return true;
        }
        return false;
    }

    WPARAM k = key.code;
    int tabCount = int(tabs_.size());

    // Anywhere in the window.
    if (ctrl && k == 'T') { newTab(tab().path, true); return true; }
    if (ctrl && k == 'W') { closeTab(active_); return true; }
    if (ctrl && k == 'S') { saveTabs(); return true; }
    if (ctrl && k == 'O') { openSavedTabs(); return true; }
    if (ctrl && k == VK_TAB) { switchTab((active_ + (shift ? tabCount - 1 : 1)) % tabCount); return true; }
    if (ctrl && k >= '1' && k <= '9') { switchTab(k == '9' ? tabCount - 1 : std::min(int(k - '1'), tabCount - 1)); return true; }
    if (ctrl && shift && (k == VK_PRIOR || k == VK_NEXT)) { moveTab(active_, k == VK_NEXT ? 1 : -1); return true; }
    if ((ctrl && k == 'L') || (alt && k == 'D') || (k == VK_F4 && !alt)) { focusEdit(address_); return true; }
    if ((ctrl && k == 'F') || k == VK_F3) { focusEdit(filter_); return true; }
    if (k == VK_F5 || (ctrl && k == 'R')) { refresh(); return true; }
    if (ctrl && k == 'H') {   // hidden items are always read: the list just shows or hides them
        list_.setShowHidden(!list_.showHidden());
        setMessage(list_.showHidden() ? L"Showing hidden files" : L"Hiding hidden files");
        return true;
    }
    if (alt && k == VK_LEFT) { goBack(); return true; }
    if (alt && k == VK_RIGHT) { goForward(); return true; }
    if (alt && k == VK_UP) { goUp(); return true; }
    if (alt && k == VK_RETURN && focus == list) {
        invokeVerb(wnd_, itemMenu(wnd_, list_.dir(), list_.selectedItems()).get(), "properties");
        return true;
    }
    if (k == VK_TAB && !ctrl && !alt) {
        HWND order[] = {address_, filter_, tree_.hwnd(), list};
        int i = int(std::find(std::begin(order), std::end(order), focus) - std::begin(order));
        HWND next = order[(i + (shift ? 3 : 1)) % 4];
        if (next == list) focusList();
        else if (next == tree_.hwnd()) SetFocus(next);
        else focusEdit(next);
        return true;
    }
    if (key.message != WM_KEYDOWN) return false;

    if (focus == address_) {
        if (k == VK_RETURN) { goToTyped(); return true; }
        if (k == VK_ESCAPE) {
            SetWindowTextW(address_, locationText(tab().path).c_str());
            focusList();
            return true;
        }
        return false;
    }
    if (focus == filter_) {
        if (k == VK_ESCAPE) { SetWindowTextW(filter_, L""); focusList(); return true; }
        if (k == VK_DOWN) { focusList(); return true; }
        if (ctrl && k == VK_BACK) { deleteWordBefore(filter_); return true; }
        if (k == VK_RETURN) {   // opens the highlighted item only (5.4), even if other matches are still selected
            list_.select({}, list_.focusedName());
            focusList();
            activate(ctrl);
            return true;
        }
        return false;
    }
    if (focus == list) {
        if (k == VK_RETURN) { activate(ctrl); return true; }
        if (k == VK_BACK) { goUp(); return true; }
        if (k == VK_DELETE) { deleteSelection(shift); return true; }
        if (k == VK_F2) {
            std::vector<int> rows = list_.selection();
            if (!rows.empty()) list_.rename(list_.focused() >= 0 && !ctrl ? list_.focused() : rows[0]);
            return true;
        }
        if (k == VK_ESCAPE && GetWindowTextLengthW(filter_) > 0) { SetWindowTextW(filter_, L""); return true; }
        if (ctrl && shift && k == 'N') { newFolder(); return true; }
        if (ctrl && k == 'A') { list_.selectAll(); return true; }
        if (ctrl && (k == 'C' || k == 'X')) {
            std::vector<Item> items = list_.selectedItems();
            if (items.empty()) return true;
            if (copyToClipboard(wnd_, list_.dir(), items, k == 'X')) {
                setMessage(std::to_wstring(items.size()) + (k == 'X' ? L" item(s) cut" : L" item(s) copied"));
            } else {
                setMessage(L"Can't put the items on the clipboard");
                warn();
            }
            return true;
        }
        if (ctrl && k == 'V') { pasteInto(wnd_, list_.dir()); return true; }
        return false;
    }
    if (focus == tree_.hwnd() && k == VK_BACK) { goUp(); return true; }
    return false;
}

void App::focusList() {
    SetFocus(list_.hwnd());
    if (list_.selection().empty() && list_.focused() >= 0)
        ListView_SetItemState(list_.hwnd(), list_.focused(), LVIS_SELECTED, LVIS_SELECTED);
}

// ---------------------------------------------------------------- navigation

void App::open(const std::wstring& path, Nav nav, ViewState view, Origin origin) {
    if (list_.renaming()) ListView_CancelEditLabel(list_.hwnd());
    // A new folder starts unfiltered (5.4): cleared now, so letters typed while it loads filter it.
    std::wstring filter = windowText(filter_);
    if (nav != Nav::Refresh && !filter.empty()) SetWindowTextW(filter_, L"");
    UINT generation = ++generation_;
    g_newestLoad = generation;
    pending_ = {generation, nav, std::move(view), tab().id, std::move(origin), std::move(filter)};
    SetTimer(wnd_, kLoadingTimer, 250, nullptr);   // say "Loading..." only if it is slow
    if (nav == Nav::Switch) {
        // Show the new tab at once; until its folder loads the list has none, so nothing acts on the old one.
        list_.setListing(Listing{});
        showLocation(nav);
        showStatus();
    }
    HWND wnd = wnd_;
    std::thread([wnd, generation, path] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        auto replaced = [generation] { return g_newestLoad != generation; };
        auto result = std::make_unique<Listing>(listFolder(path, true, replaced));
        std::vector<std::wstring> own;
        for (const Item& it : result->items)
            if (hasOwnIcon(it)) own.push_back(it.name);
        if (PostMessageW(wnd, WM_APP_LOADED, generation, reinterpret_cast<LPARAM>(result.get()))) result.release();
        // Then the icons that need a disk (or network) lookup, so they never hold up the list.
        if (!own.empty() && !replaced()) {
            auto icons = std::make_unique<IconBatch>(readOwnIcons(path, own));
            if (PostMessageW(wnd, WM_APP_ICONS, 0, reinterpret_cast<LPARAM>(icons.get()))) icons.release();
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}

void App::onLoaded(Listing&& listing, UINT generation) {
    ++loadsDone_;
    if (generation != generation_ || generation != pending_.generation) {   // superseded
        if (listing.error != ERROR_OPERATION_ABORTED) ++loadsWasted_;
        return;
    }
    KillTimer(wnd_, kLoadingTimer);
    if (loadMessageShown_) {   // any load, not only navigation, may have shown one
        setMessage(L"");
        loadMessageShown_ = false;
    }
    Pending p = std::exchange(pending_, {});
    if (tab().id != p.tabId) return;
    if (p.nav == Nav::Refresh && list_.renaming()) {
        // 4.2: never apply a refresh under an open rename box (Enter would rename whatever row moved there).
        // Retry once it closes, keeping what this load was asked to select unless a newer request waits.
        if (p.view.asks() && !queuedView_.view.asks()) queuedView_ = {list_.dir(), std::move(p.view)};
        queueRefresh();
        return;
    }
    Tab& t = tab();

    if (listing.error) return onLoadFailed(listing, p);

    std::wstring old = t.path;
    bool sameFolder = samePath(list_.dir(), listing.path);
    switch (p.nav) {
    case Nav::New:
        if (!samePath(old, listing.path)) {
            t.back.push_back(old);
            t.forward.clear();
        }
        break;
    case Nav::Back:
        if (!t.back.empty()) t.back.pop_back();
        t.forward.push_back(old);
        break;
    case Nav::Forward:
        if (!t.forward.empty()) t.forward.pop_back();
        t.back.push_back(old);
        break;
    case Nav::Refresh:
    case Nav::Switch:
        break;
    }
    if (t.back.size() > kMaxHistory) t.back.erase(t.back.begin());
    if (t.forward.size() > kMaxHistory) t.forward.erase(t.forward.begin());

    ViewState view = std::move(p.view);
    // An item a rename, delete or new folder asked for (a tab switch restores the scroll exactly instead).
    bool requested = p.nav == Nav::Refresh && view.asks();
    bool moved = p.nav == Nav::New || p.nav == Nav::Back || p.nav == Nav::Forward;
    if (p.nav == Nav::Refresh && sameFolder) {
        if (!view.asks()) view = currentView();
        if (view.top < 0) view.top = list_.topRow();
    }
    if (moved && !view.asks()) {
        // Going up (or back to a parent) selects the folder you came from.
        std::wstring child = childToward(listing.path, old);
        if (!child.empty()) view = ViewState::selecting(child);
    }

    t.path = listing.path;
    t.unavailable = false;
    list_.setListing(std::move(listing));
    int row = list_.select(view.select, view.focus);
    if (row < 0 && !requested && view.focusRow >= 0 && list_.count() > 0)   // the focused item is gone (4.5):
        row = list_.focusRow(std::min(view.focusRow, list_.count() - 1));   // the item in its place takes the focus
    if (view.top >= 0) list_.scrollToRow(view.top);   // a refresh keeps the scroll position (4.5)...
    if (view.top < 0 || requested) list_.ensureVisible(row);   // ...but what the user just made or renamed shows
    if (view.rename && row >= 0) list_.rename(row);

    watch_ = FolderWatch(wnd_, WM_APP_CHANGED, t.path);
    showLocation(p.nav);
    if (moved) setMessage(L"");
    showStatus();
}

// The address bar, title, buttons and tabs follow the current tab's folder.
void App::showLocation(Nav nav) {
    const Tab& t = tab();
    if (nav != Nav::Refresh || GetFocus() != address_)   // never overwrite what the user is typing
        SetWindowTextW(address_, locationText(t.path).c_str());
    SetWindowTextW(wnd_, (displayName(t.path) + L" - " + kAppName).c_str());
    EnableWindow(back_, !t.back.empty());
    EnableWindow(forward_, !t.forward.empty());
    EnableWindow(up_, !t.path.empty());
    showTabs();
}

// The folder could not be read: decide what to show or do instead.
void App::onLoadFailed(const Listing& listing, const Pending& p) {
    Tab& t = tab();
    // A navigation that fails or opens a file leaves the folder shown: its filter comes back, unless retyped.
    if (p.nav != Nav::Refresh && p.nav != Nav::Switch && GetWindowTextLengthW(filter_) == 0)
        SetWindowTextW(filter_, p.filter.c_str());
    // A shortcut that leads to a file: Windows opens it (never an unreachable one: that would hang, then fail).
    if (!p.origin.shortcut.empty() && listing.isFile) {
        openFile(wnd_, p.origin.shortcut);
        return;
    }
    if (p.origin.typed && listing.isFile) {   // a file typed in the address bar: open it
        openFile(wnd_, listing.path);
        SetWindowTextW(address_, locationText(t.path).c_str());
        return;
    }
    // Let the user fix what they typed (5.3), unless they moved on: a dead share fails a minute later.
    if (p.origin.typed && GetFocus() == list_.hwnd()) focusEdit(address_);
    if (p.nav == Nav::Refresh || p.nav == Nav::Switch) {
        // A tab never changes its folder by itself (a drive may come back): show it empty and say why. The
        // list gets no folder, so nothing asks Windows about it (that can freeze the window for 20-40 s).
        list_.setListing(Listing{});
        t.unavailable = true;
        watch_ = {};
        showLocation(p.nav);
        setMessage(L"\"" + locationText(listing.path) + L"\" is not available: " + errorText(listing.error) +
                   L"   (F5 tries again)");
        loadMessageShown_ = true;
        showStatus();
        return;
    }
    // A history entry whose folder is gone is dropped, so Back/Forward can move past it.
    std::vector<std::wstring>& history = p.nav == Nav::Back ? t.back : t.forward;
    if ((p.nav == Nav::Back || p.nav == Nav::Forward) && !history.empty() && samePath(history.back(), listing.path)) {
        history.pop_back();
        EnableWindow(back_, !t.back.empty());
        EnableWindow(forward_, !t.forward.empty());
    }
    setMessage(displayName(listing.path) + L": " + errorText(listing.error));
    warn();
}

void App::goBack() {
    if (!tab().back.empty()) open(tab().back.back(), Nav::Back);
}

void App::goForward() {
    if (!tab().forward.empty()) open(tab().forward.back(), Nav::Forward);
}

void App::goUp() {
    if (!tab().path.empty()) open(parentPath(tab().path), Nav::New);
}

void App::refresh(ViewState view) {
    // A load is in progress: start nothing (a held F5 would pile up stuck threads on a dead share). That load
    // may predate the caller's change, so keep what it wants selected for a queued refresh.
    if (pending_.generation) {
        if (view.asks()) {
            queuedView_ = {list_.dir(), std::move(view)};
            queueRefresh();
        }
        return;
    }
    if (view.asks()) queuedView_ = {};   // newer than any request still waiting
    open(tab().path, Nav::Refresh, std::move(view));
}

void App::goToTyped() {
    std::wstring text = windowText(address_);
    std::wstring path = normalizePath(expandVars(text));   // only what the user typed has its %VARS% expanded
    // "..", subfolder names and relative paths (".\x", "a\b") open here: Windows would use Explorer (4.4).
    bool relative = isRelativePath(path);
    if (path == L"..") path = parentPath(tab().path);
    else if (!tab().path.empty() && (list_.hasFolder(path) || relative))
        path = normalizePath(joinPath(tab().path, path));   // GetFullPathName folds "." and ".." away
    if (path.empty() || isAbsolutePath(path)) {
        // Checked on the loader's thread, so a dead share cannot freeze the window; a file opens then.
        open(path, Nav::New, {}, Origin::addressBar());
        focusList();
        return;
    }
    // A command ("cmd", a web address...), run in the folder shown: a dead tab folder would hold up its start.
    // On This PC a relative place has no folder (Windows would open its own start folder in Explorer).
    if ((relative && tab().path.empty()) || !runCommand(wnd_, text, list_.dir())) {
        setMessage(L"Can't find \"" + text + L"\"");
        focusEdit(address_);   // left selected, like a bad path, so it can be corrected (5.3)
        warn();
        return;
    }
    SetWindowTextW(address_, locationText(tab().path).c_str());
    focusList();
}

void App::activate(bool inNewTab) {
    std::vector<int> rows = list_.selection();
    if (rows.empty() && list_.focused() >= 0) rows = {list_.focused()};
    if (rows.size() == 1 && !list_.at(rows[0]).isDir() && isShortcut(list_.at(rows[0]).name)) {
        openShortcut(list_.pathAt(rows[0]), inNewTab);
        return;
    }
    std::vector<std::wstring> dirs, files;
    for (int row : rows) {
        std::wstring path = list_.pathAt(row);
        bool folder = list_.at(row).isDir();
        // A shortcut to a folder counts as that folder, so it opens in a tab (Windows would use Explorer).
        if (!folder && extensionOf(path) == L".lnk") {
            std::optional<std::wstring> target = shortcutTarget(path, &folder);
            if (folder && opensInTab(target)) path = normalizePath(*target);   // as openShortcut() does (L7)
            else folder = false;
        }
        (folder ? dirs : files).push_back(path);
    }
    if (files.size() > 15 &&
        MessageBoxW(wnd_, (L"Open " + std::to_wstring(files.size()) + L" files?").c_str(), kAppName,
                    MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    for (const auto& f : files) openFile(wnd_, f);
    if (dirs.size() == 1 && !inNewTab) {
        open(dirs[0], Nav::New);
    } else {
        for (const auto& d : dirs) newTab(d, false);   // several folders: background tabs
    }
}

// A shortcut to a folder or This PC opens here, or in a background tab; Windows opens anything else.
// Whether the target really is a folder, the loader finds out on its thread (it may be on a network).
void App::openShortcut(const std::wstring& path, bool inNewTab) {
    bool folder = false;
    std::optional<std::wstring> target = extensionOf(path) == L".lnk" ? shortcutTarget(path, &folder) : std::nullopt;
    if (!opensInTab(target)) {
        openFile(wnd_, path);   // a web link, a program, a library...
        return;
    }
    // Normalized like every way into a tab: a stored "D:\photos\" would give a blank caption and a stuck Up.
    std::wstring dir = normalizePath(*target);
    if (inNewTab && folder) newTab(dir, false);
    else open(dir, Nav::New, {}, Origin::fromShortcut(path));
}

// Rename on a tree node's menu happens in the file list, in the node's parent folder.
void App::renameNode(const std::wstring& path) {
    if (parentPath(path).empty()) return;   // This PC, a drive or a share: nothing to rename here
    ViewState v = ViewState::selecting(leafName(path));
    v.rename = true;
    open(parentPath(path), Nav::New, v);
}

// A node in the tree: a folder, or a bookmark shortcut.
void App::openNode(const std::wstring& path, bool inNewTab) {
    if (isShortcut(path)) openShortcut(path, inNewTab);
    else if (inNewTab) newTab(path, false);
    else open(path, Nav::New);
}

ViewState App::currentView() const {
    ViewState v;
    v.select = list_.selectedNames();
    v.focus = list_.focusedName();
    v.focusRow = list_.focused();
    v.top = list_.topRow();
    return v;
}

// Not while the tab's folder is still loading after a switch: that would forget what it showed before.
void App::rememberView() {
    if (pending_.generation && pending_.nav == Nav::Switch) return;
    SetWindowTextW(filter_, L"");   // the tab comes back unfiltered (5.4): remember the unfiltered rows
    if (samePath(list_.dir(), tab().path)) tab().view = currentView();
}

// ---------------------------------------------------------------- tabs

Tab App::makeTab(const std::wstring& path) {
    Tab t;
    t.id = nextTabId_++;
    t.path = path;
    return t;
}

void App::newTab(std::wstring path, bool switchTo) {
    int at = active_ + 1;
    tabs_.insert(tabs_.begin() + at, makeTab(path));
    if (switchTo) {
        rememberView();
        active_ = at;
        open(path, Nav::Switch);
    }
    showTabs();
}

void App::closeTab(int index) {
    if (tabs_.size() <= 1 || index < 0 || index >= int(tabs_.size())) return;
    bool wasActive = index == active_;
    tabs_.erase(tabs_.begin() + index);
    if (index < active_) --active_;
    active_ = std::min(active_, int(tabs_.size()) - 1);
    showTabs();
    if (wasActive) open(tab().path, Nav::Switch, tab().view);
}

void App::closeOtherTabs(int index) {
    if (index < 0 || index >= int(tabs_.size())) return;
    bool wasActive = index == active_;
    Tab keep = std::move(tabs_[size_t(index)]);
    tabs_.clear();
    tabs_.push_back(std::move(keep));
    active_ = 0;
    showTabs();
    if (!wasActive) open(tab().path, Nav::Switch, tab().view);
}

void App::switchTab(int index) {
    if (index < 0 || index >= int(tabs_.size()) || index == active_) {
        showTabs();
        return;
    }
    rememberView();
    active_ = index;
    showTabs();
    open(tab().path, Nav::Switch, tab().view);
}

// Swaps a tab with its neighbor. The current tab stays current, and each tab takes its folder, history and view
// along: nothing is reloaded.
void App::moveTab(int index, int by) {
    int to = index + by;
    if (std::min(index, to) < 0 || std::max(index, to) >= int(tabs_.size())) return;
    std::swap(tabs_[size_t(index)], tabs_[size_t(to)]);
    if (active_ == index) active_ = to;
    else if (active_ == to) active_ = index;
    showTabs();
}

// ---------------------------------------------------------------- saved tabs (Bookmarks\Tabs)

// Both work on a worker thread: a tab's folder may be on a server that is off, and so may Bookmarks.
void App::saveTabs() {
    std::vector<std::wstring> folders;
    for (const Tab& t : tabs_) folders.push_back(t.path);
    std::thread([wnd = wnd_, dir = joinPath(tree_.bookmarks(), L"Tabs"), folders = std::move(folders)] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        bool ok = saveFolderShortcuts(dir, folders);
        if (SUCCEEDED(co)) CoUninitialize();
        PostMessageW(wnd, WM_APP_TABS_SAVED, ok, 0);
    }).detach();
}

void App::openSavedTabs() {
    std::thread([wnd = wnd_, dir = joinPath(tree_.bookmarks(), L"Tabs")] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        auto folders = std::make_unique<std::vector<std::wstring>>(savedFolders(dir));
        if (SUCCEEDED(co)) CoUninitialize();
        if (PostMessageW(wnd, WM_APP_TABS_READ, 0, reinterpret_cast<LPARAM>(folders.get()))) folders.release();
    }).detach();
}

// After the open tabs, each folder that isn't open yet; the first of them is shown.
void App::addSavedTabs(const std::vector<std::wstring>& folders) {
    int first = int(tabs_.size());
    for (const std::wstring& f : folders)
        if (std::none_of(tabs_.begin(), tabs_.end(), [&](const Tab& t) { return samePath(t.path, f); }))
            tabs_.push_back(makeTab(f));
    if (first < int(tabs_.size())) switchTab(first);
    else setMessage(folders.empty() ? L"No saved tabs in Bookmarks\\Tabs" : L"The saved tabs are open already");
}

// The folder of the tab under the cursor, for a drop. That tab lights up while hovered.
std::optional<std::wstring> App::tabDropTarget(const POINT* screenPt) {
    int hover = -1;
    if (screenPt) {
        TCHITTESTINFO hit{};
        hit.pt = *screenPt;
        ScreenToClient(tabBar_, &hit.pt);
        hover = TabCtrl_HitTest(tabBar_, &hit);
    }
    if (hover != dropTab_) {
        if (dropTab_ >= 0) TabCtrl_HighlightItem(tabBar_, dropTab_, FALSE);
        if (hover >= 0) TabCtrl_HighlightItem(tabBar_, hover, TRUE);
        dropTab_ = hover;
    }
    if (hover < 0 || hover >= int(tabs_.size())) return std::nullopt;
    const Tab& t = tabs_[size_t(hover)];
    // Not into This PC, not into a folder that has not loaded here, and not back where the files come from.
    if (t.path.empty() || t.unavailable || (draggingFiles_ && samePath(t.path, list_.dir()))) return std::nullopt;
    return t.path;
}

void App::showTabs() {
    dropTab_ = -1;   // the tabs are rebuilt, so no highlight is left
    TabCtrl_DeleteAllItems(tabBar_);
    for (size_t i = 0; i < tabs_.size(); ++i) {
        std::wstring text = displayName(tabs_[i].path);
        if (text.size() > 28) {
            size_t cut = IS_HIGH_SURROGATE(text[26]) ? 26 : 27;   // never between the two halves of an emoji
            text = text.substr(0, cut) + L"…";
        }
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = text.data();
        TabCtrl_InsertItem(tabBar_, int(i), &item);
    }
    TabCtrl_SetCurSel(tabBar_, active_);
}

// ---------------------------------------------------------------- file commands

void App::deleteSelection(bool permanently) {
    std::vector<int> rows = list_.selection();
    if (rows.empty() || list_.dir().empty()) return;
    // Afterwards, select the item that followed the deleted ones.
    int next = rowAfterDeleting(rows, list_.count());
    ViewState v = next >= 0 ? ViewState::selecting(list_.at(next).name) : ViewState{};
    v.top = list_.topRow();
    // No to Windows' question deletes nothing: the selection stays. After a delete nothing is selected until
    // the reload lands, so a second Delete cannot hit the deleted files.
    if (deleteItems(wnd_, list_.dir(), list_.selectedItems(), permanently)) { list_.select({}, v.focus); refresh(v); }
}

void App::newFolder() {
    const std::wstring& dir = list_.dir();   // the folder shown (none for This PC or an unavailable folder)
    if (dir.empty()) return;
    std::wstring name = L"New folder";
    for (int i = 2; !CreateDirectoryW(joinPath(dir, name).c_str(), nullptr); ++i) {
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS || i > 999) {
            setMessage(errorText(error));
            warn();   // 4.9: an impossible action beeps
            return;
        }
        name = L"New folder (" + std::to_wstring(i) + L")";
    }
    SetWindowTextW(filter_, L"");
    ViewState v = ViewState::selecting(name);
    v.rename = true;
    refresh(v);
}

// ---------------------------------------------------------------- status and settings

void App::showStatus() {
    std::vector<int> rows = list_.selection();
    uint64_t bytes = 0;
    int files = 0;
    for (int row : rows) {
        if (list_.at(row).isDir()) continue;
        bytes += list_.at(row).size;
        ++files;
    }
    std::wstring s = std::to_wstring(list_.count()) + (list_.count() == 1 ? L" item" : L" items");
    if (list_.count() != list_.total()) s += L" (of " + std::to_wstring(list_.total()) + L")";
    if (!rows.empty()) {
        s += L"     " + std::to_wstring(rows.size()) + L" selected";
        if (files) s += L"  " + byteSize(bytes);
    }
    SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(s.c_str()));
}

void App::warn() {
    if (!selfTest_) MessageBeep(MB_ICONWARNING);   // self-tests trigger errors on purpose; keep them quiet
}

void App::setMessage(const std::wstring& text) {
    SendMessageW(status_, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(text.c_str()));
}

void App::loadSettings(const CommandLine& cmd) {
    // Missing, not a number or far off any screen (a damaged file: x + width must not overflow): the default.
    auto number = [&](const std::wstring& key, int fallback) {
        wchar_t text[32] = {};
        GetPrivateProfileStringW(L"Window", key.c_str(), L"", text, 32, ini_.c_str());
        wchar_t* end = nullptr;
        long value = wcstol(text, &end, 10);
        return end != text && *end == 0 && value >= -100000 && value <= 100000 ? int(value) : fallback;
    };

    treeWidth_ = number(L"TreeWidth", 220);
    list_.setShowHidden(number(L"ShowHidden", 0) != 0);
    list_.setSort(SortKey(std::clamp(number(L"SortColumn", 0), 0, 3)), number(L"SortAscending", 1) != 0);
    const int defaults[] = {320, 150, 170, 90};
    std::vector<int> widths;
    for (int i = 0; i < 4; ++i)
        widths.push_back(scale(number(L"Column" + std::to_wstring(i), defaults[i]), dpi_));
    list_.setColumnWidths(widths);

    // Tabs are not remembered: the command line's folders, or the home folder.
    for (const auto& folder : cmd.folders) tabs_.push_back(makeTab(folder));
    if (tabs_.empty()) tabs_.push_back(makeTab(knownFolder(FOLDERID_Profile)));
    active_ = 0;

    int w = number(L"Width", 0), h = number(L"Height", 0);
    if (w > 0 && h > 0) {
        WINDOWPLACEMENT wp{};
        wp.length = sizeof wp;
        int x = number(L"X", 0), y = number(L"Y", 0);
        wp.rcNormalPosition = {x, y, x + w, y + h};
        wp.showCmd = number(L"Maximized", 0) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        // A position on a monitor that is gone (a laptop undocked) would put the window out of sight.
        if (MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONULL)) SetWindowPlacement(wnd_, &wp);
        else ShowWindow(wnd_, wp.showCmd);
    } else {
        ShowWindow(wnd_, useSettings_ ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);   // a self-test never steals focus
    }
}

void App::saveSettings() {
    if (!useSettings_) return;   // only numbers are saved: a plain ini file is fine
    auto number = [&](const std::wstring& key, int value) {
        WritePrivateProfileStringW(L"Window", key.c_str(), std::to_wstring(value).c_str(), ini_.c_str());
    };

    WINDOWPLACEMENT wp{};
    wp.length = sizeof wp;
    GetWindowPlacement(wnd_, &wp);
    const RECT& r = wp.rcNormalPosition;
    number(L"X", r.left);
    number(L"Y", r.top);
    number(L"Width", r.right - r.left);
    number(L"Height", r.bottom - r.top);
    bool maximized = wp.showCmd == SW_SHOWMAXIMIZED || (wp.flags & WPF_RESTORETOMAXIMIZED);   // also when minimized
    number(L"Maximized", maximized ? 1 : 0);
    number(L"TreeWidth", treeWidth_);
    number(L"ShowHidden", list_.showHidden() ? 1 : 0);
    number(L"SortColumn", int(list_.sortKey()));
    number(L"SortAscending", list_.sortAscending() ? 1 : 0);
    std::vector<int> widths = list_.columnWidths();
    for (size_t i = 0; i < widths.size(); ++i)
        number(L"Column" + std::to_wstring(i), MulDiv(widths[i], 96, int(dpi_)));
}
