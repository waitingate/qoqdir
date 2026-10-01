#include "app.h"

#include "shell.h"

#include <dbt.h>
#include <windowsx.h>

#include <algorithm>
#include <optional>
#include <thread>

namespace {

enum : int { ID_BACK = 100, ID_FORWARD, ID_UP, ID_ADDRESS, ID_FILTER, ID_TABS, ID_PLUS, ID_TREE, ID_LIST, ID_STATUS };
enum : UINT {
    WM_APP_LOADED = WM_APP + 1, WM_APP_CHANGED, WM_APP_CLOSETAB, WM_APP_RENAME, WM_APP_ICONS, WM_APP_TREE,
    WM_APP_MIDDLECLICK
};

constexpr size_t kMaxHistory = 100;
constexpr COLORREF kBackground = RGB(235, 235, 235);   // soft gray: less glare than white

bool is(const std::wstring& verb, const wchar_t* name) { return _wcsicmp(verb.c_str(), name) == 0; }

std::wstring windowText(HWND h) {
    std::wstring s(size_t(GetWindowTextLengthW(h)) + 1, L'\0');
    s.resize(size_t(GetWindowTextW(h, s.data(), int(s.size()))));
    return s;
}

std::wstring byteSize(uint64_t bytes) {
    wchar_t buf[64];
    return StrFormatByteSizeW(static_cast<LONGLONG>(bytes), buf, 64) ? buf : L"";
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
    saveTabs_ = !cmd.onlyThese;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
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
    tree_.create(wnd_, ID_TREE, WM_APP_TREE);
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

    std::wstring exe(32768, L'\0');   // not MAX_PATH: the app may live in a deep folder
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size())));
    // Without settings (self-test), read from a file that does not exist: every value is a default.
    ini_ = useSettings_ ? joinPath(parentPath(exe), std::wstring(kAppName) + L".ini") : L"?";
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

    int splitW = scale(5, dpi_);
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
    case WM_SIZE:
        layout();
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
        // At most one refresh per interval, even while a folder changes nonstop.
        if (!refreshQueued_) {
            refreshQueued_ = true;
            SetTimer(wnd_, kRefreshTimer, 400, nullptr);
        }
        return 0;
    case WM_APP_ICONS: {
        std::unique_ptr<IconBatch> batch(reinterpret_cast<IconBatch*>(lp));
        rememberIcons(*batch);
        if (samePath(list_.dir(), batch->dir)) InvalidateRect(list_.hwnd(), nullptr, FALSE);
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
    case WM_APP_CLOSETAB:
        closeTab(int(wp));
        return 0;
    case WM_APP_RENAME: {
        Rename r = std::move(rename_);
        rename_ = {};
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
            tree_.fill();
            if (tab().path.empty() && !pending_.generation) refresh();
        }
        return TRUE;
    case WM_SETCURSOR:
        if (reinterpret_cast<HWND>(wp) == wnd_ && LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(wnd_, &p);
            if (p.x >= splitterX_ && p.x < splitterX_ + scale(5, dpi_)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp);
        if (x >= splitterX_ && x < splitterX_ + scale(5, dpi_)) {
            dragging_ = true;
            SetCapture(wnd_);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (dragging_) {
            treeWidth_ = MulDiv(std::max(int(GET_X_LPARAM(lp)), 0), 96, int(dpi_));
            layout();
        }
        return 0;
    case WM_LBUTTONUP:
        if (dragging_) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        dragging_ = false;
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

void App::onTimer(UINT_PTR id) {
    if (id == kRefreshTimer) {
        // Try again later: never disturb a rename, and never cancel a folder that is loading.
        if (list_.renaming() || pending_.generation) return;
        KillTimer(wnd_, kRefreshTimer);
        refreshQueued_ = false;
        refresh();
    } else if (id == kStatusTimer) {
        KillTimer(wnd_, kStatusTimer);
        showStatus();
    } else if (id == kLoadingTimer) {
        KillTimer(wnd_, kLoadingTimer);
        if (pending_.generation) {   // say "Loading..." only if it is slow
            setMessage(L"Loading…");
            loadingShown_ = true;
        }
    } else if (id == kSelfTestTimer) {
        selfTestTick();
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
        newTab(path, false);
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
            std::vector<std::wstring> paths = list_.selectedPaths();
            if (!paths.empty()) {
                draggingFiles_ = true;
                dragOut(wnd_, paths);
                draggingFiles_ = false;
            }
            break;
        }
        }
        return 0;
    }
    if (nm->hwndFrom == tree_.hwnd()) {
        std::wstring path;
        if (tree_.onNotify(nm, &path)) open(path, Nav::New);
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
        if (rows.empty()) {
            if (!list_.dir().empty()) showMenu(wnd_, backgroundMenu(wnd_, list_.dir()).get(), pt, nullptr);
            return;
        }
        bool allDirs = std::all_of(rows.begin(), rows.end(), [&](int r) { return list_.at(r).isDir(); });
        showMenu(wnd_, itemMenu(wnd_, list_.selectedPaths()).get(), pt, [&](const std::wstring& verb) {
            if (is(verb, L"rename")) {
                list_.rename(list_.focused() >= 0 ? list_.focused() : rows[0]);
                return true;
            }
            // Folders open here, not in a separate Explorer window.
            if (allDirs && (is(verb, L"open") || is(verb, L"explore"))) {
                activate(false);
                return true;
            }
            if (allDirs && (is(verb, L"opennewtab") || is(verb, L"opennewwindow"))) {
                activate(true);
                return true;
            }
            return false;
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
        showMenu(wnd_, itemMenu(wnd_, {path}).get(), pt, [&](const std::wstring& verb) {
            if (is(verb, L"open") || is(verb, L"explore")) open(path, Nav::New);
            else if (is(verb, L"opennewtab") || is(verb, L"opennewwindow")) newTab(path, true);
            else return is(verb, L"rename");   // renaming is done in the file list
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
    int cmd = int(TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, wnd_, nullptr));
    if (cmd == 1) newTab(tabs_[size_t(index)].path, true);
    if (cmd == 2) closeTab(index);
    if (cmd == 3) closeOtherTabs(index);
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
    auto focusEdit = [](HWND edit) {
        SetFocus(edit);
        SendMessageW(edit, EM_SETSEL, 0, -1);
    };

    // Anywhere in the window.
    if (ctrl && k == 'T') { newTab(tab().path, true); return true; }
    if (ctrl && k == 'W') { closeTab(active_); return true; }
    if (ctrl && k == VK_TAB) { switchTab((active_ + (shift ? tabCount - 1 : 1)) % tabCount); return true; }
    if (ctrl && k >= '1' && k <= '9') { switchTab(k == '9' ? tabCount - 1 : std::min(int(k - '1'), tabCount - 1)); return true; }
    if ((ctrl && k == 'L') || (alt && k == 'D') || (k == VK_F4 && !alt)) { focusEdit(address_); return true; }
    if ((ctrl && k == 'F') || k == VK_F3) { focusEdit(filter_); return true; }
    if (k == VK_F5 || (ctrl && k == 'R')) { refresh(); return true; }
    if (ctrl && k == 'H') {
        showHidden_ = !showHidden_;
        setMessage(showHidden_ ? L"Showing hidden files" : L"Hiding hidden files");
        reload();
        return true;
    }
    if (alt && k == VK_LEFT) { goBack(); return true; }
    if (alt && k == VK_RIGHT) { goForward(); return true; }
    if (alt && k == VK_UP) { goUp(); return true; }
    if (alt && k == VK_RETURN && focus == list) {
        invokeVerb(wnd_, itemMenu(wnd_, list_.selectedPaths()).get(), "properties");
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
        if (k == VK_RETURN) { focusList(); activate(ctrl); return true; }
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
            std::vector<std::wstring> paths = list_.selectedPaths();
            if (!paths.empty()) {
                copyToClipboard(wnd_, paths, k == 'X');
                setMessage(std::to_wstring(paths.size()) + (k == 'X' ? L" item(s) cut" : L" item(s) copied"));
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
    UINT generation = ++generation_;
    pending_ = {generation, path, nav, std::move(view), tab().id, std::move(origin)};
    SetTimer(wnd_, kLoadingTimer, 250, nullptr);   // say "Loading..." only if it is slow
    HWND wnd = wnd_;
    bool hidden = showHidden_;
    std::thread([wnd, generation, path, hidden] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        auto result = std::make_unique<Listing>(listFolder(path, hidden));
        std::vector<std::wstring> own;
        for (const Item& it : result->items)
            if (hasOwnIcon(it)) own.push_back(it.name);
        if (PostMessageW(wnd, WM_APP_LOADED, generation, reinterpret_cast<LPARAM>(result.get()))) result.release();
        // Then the icons that need a disk (or network) lookup, so they never hold up the list.
        if (!own.empty()) {
            auto icons = std::make_unique<IconBatch>(readOwnIcons(path, own));
            if (PostMessageW(wnd, WM_APP_ICONS, 0, reinterpret_cast<LPARAM>(icons.get()))) icons.release();
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}

void App::onLoaded(Listing&& listing, UINT generation) {
    if (generation != generation_ || generation != pending_.generation) return;   // superseded
    KillTimer(wnd_, kLoadingTimer);
    if (loadingShown_) {   // any load, not only navigation, may have shown it
        setMessage(L"");
        loadingShown_ = false;
    }
    Pending p = std::move(pending_);
    pending_ = {};
    if (tab().id != p.tabId) return;
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
    bool moved = p.nav == Nav::New || p.nav == Nav::Back || p.nav == Nav::Forward;
    if (p.nav == Nav::Refresh && sameFolder) {
        if (view.select.empty() && view.focus.empty()) view = currentView();
        if (view.top < 0) view.top = list_.topRow();
    }
    if (moved && view.select.empty() && view.focus.empty()) {
        // Going up (or back to a parent) selects the folder you came from.
        std::wstring child = childToward(listing.path, old);
        if (!child.empty()) view = ViewState::selecting(child);
    }
    if (!(p.nav == Nav::Refresh && sameFolder) && GetWindowTextLengthW(filter_) > 0) SetWindowTextW(filter_, L"");

    t.path = listing.path;
    list_.setListing(std::move(listing));
    int row = list_.select(view.select, view.focus);
    if (view.top >= 0) list_.scrollToRow(view.top);
    else list_.ensureVisible(row);
    if (view.rename && row >= 0) list_.rename(row);

    watch_ = FolderWatch(wnd_, WM_APP_CHANGED, t.path);
    if (p.nav != Nav::Refresh || GetFocus() != address_)   // never overwrite what the user is typing
        SetWindowTextW(address_, locationText(t.path).c_str());
    SetWindowTextW(wnd_, (displayName(t.path) + L" - " + kAppName).c_str());
    EnableWindow(back_, !t.back.empty());
    EnableWindow(forward_, !t.forward.empty());
    EnableWindow(up_, !t.path.empty());
    showTabs();
    if (moved) setMessage(L"");
    showStatus();
}

// The folder could not be read: decide what to show or do instead.
void App::onLoadFailed(const Listing& listing, const Pending& p) {
    Tab& t = tab();
    if (!p.origin.shortcut.empty()) {   // the shortcut does not lead to a folder: let Windows open it
        openFile(wnd_, p.origin.shortcut);
        return;
    }
    if (p.origin.typed && listing.isFile) {   // a file typed in the address bar: open it
        openFile(wnd_, listing.path);
        SetWindowTextW(address_, locationText(t.path).c_str());
        return;
    }
    if (p.origin.typed) {   // let the user correct what they typed
        SetFocus(address_);
        SendMessageW(address_, EM_SETSEL, 0, -1);
    }
    if ((p.nav == Nav::Refresh || p.nav == Nav::Switch) && !listing.path.empty()) {
        // The tab's own folder is gone (deleted, unplugged...): fall back to its parent.
        setMessage(L"\"" + listing.path + L"\" is not available: " + errorText(listing.error));
        open(parentPath(listing.path), p.nav);
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
    // A folder is already loading, and what it shows will be fresh: start nothing new.
    // (Refreshing the old folder would cancel it; restarting it on every held-down F5
    // would pile up stuck threads on a slow network share.)
    if (pending_.generation) return;
    open(tab().path, Nav::Refresh, std::move(view));
}

void App::reload() {
    if (!pending_.generation) return refresh();
    Pending p = pending_;   // restart the load in progress, e.g. with new settings
    open(p.path, p.nav, std::move(p.view), std::move(p.origin));
}

void App::goToTyped() {
    std::wstring text = windowText(address_);
    std::wstring path = normalizePath(text);
    if (path.empty() || isAbsolutePath(path)) {
        // Checked by the folder loader on its worker thread, so a dead network
        // share cannot freeze the window. A file is opened when the result comes back.
        open(path, Nav::New, {}, Origin::addressBar());
        focusList();
        return;
    }
    if (!runCommand(wnd_, text, tab().path)) {   // "cmd", "notepad", a web address...
        setMessage(L"Can't find \"" + text + L"\"");
        warn();
        return;
    }
    SetWindowTextW(address_, locationText(tab().path).c_str());
    focusList();
}

void App::activate(bool inNewTab) {
    std::vector<int> rows = list_.selection();
    if (rows.empty() && list_.focused() >= 0) rows = {list_.focused()};
    // One shortcut: if it leads to a folder, show that folder here. Whether it does is
    // found out by the folder loader on its worker thread (the target may be on a network).
    if (rows.size() == 1 && !inNewTab && extensionOf(list_.at(rows[0]).name) == L".lnk") {
        std::wstring lnk = list_.pathAt(rows[0]);
        std::wstring target = shortcutTarget(lnk);
        if (isAbsolutePath(target)) {
            open(target, Nav::New, {}, Origin::fromShortcut(lnk));
        } else {
            openFile(wnd_, lnk);
        }
        return;
    }
    std::vector<std::wstring> dirs, files;
    for (int row : rows) (list_.at(row).isDir() ? dirs : files).push_back(list_.pathAt(row));
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

ViewState App::currentView() const {
    ViewState v;
    if (!samePath(list_.dir(), tabs_[size_t(active_)].path)) return v;
    v.select = list_.selectedNames();
    v.focus = list_.focusedName();
    v.top = list_.topRow();
    return v;
}

// ---------------------------------------------------------------- tabs

Tab App::makeTab(const std::wstring& path) {
    Tab t;
    t.id = nextTabId_++;
    t.path = path;
    return t;
}

void App::newTab(const std::wstring& path, bool switchTo) {
    int at = active_ + 1;
    tabs_.insert(tabs_.begin() + at, makeTab(path));
    if (switchTo) {
        tabs_[size_t(active_)].view = currentView();
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
    tab().view = currentView();
    active_ = index;
    showTabs();
    open(tab().path, Nav::Switch, tab().view);
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
    const std::wstring& dir = tabs_[size_t(hover)].path;
    // Not into This PC, and not back into the folder the files are dragged from.
    if (dir.empty() || (draggingFiles_ && samePath(dir, list_.dir()))) return std::nullopt;
    return dir;
}

void App::showTabs() {
    dropTab_ = -1;   // the tabs are rebuilt, so no highlight is left
    TabCtrl_DeleteAllItems(tabBar_);
    for (size_t i = 0; i < tabs_.size(); ++i) {
        std::wstring text = displayName(tabs_[i].path);
        if (text.size() > 28) text = text.substr(0, 27) + L"…";
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
    int next = rows.back() + 1 < list_.count() ? rows.back() + 1 : rows.front() - 1;
    ViewState v = next >= 0 && next < list_.count() ? ViewState::selecting(list_.at(next).name) : ViewState{};
    v.top = list_.topRow();
    deleteItems(wnd_, list_.selectedPaths(), permanently);
    refresh(v);
}

void App::newFolder() {
    const std::wstring& dir = tab().path;
    if (dir.empty()) return;
    std::wstring name = L"New folder";
    for (int i = 2; !CreateDirectoryW(joinPath(dir, name).c_str(), nullptr); ++i) {
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS || i > 999) {
            setMessage(errorText(error));
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
    // Settings from before the app was renamed: Cubic.ini becomes Qoqdir.ini, once.
    if (useSettings_) CopyFileW(joinPath(parentPath(ini_), L"Cubic.ini").c_str(), ini_.c_str(), TRUE);
    auto read = [&](const wchar_t* section, const std::wstring& key) -> std::optional<std::wstring> {
        std::wstring buf(32768, L'\0');
        GetPrivateProfileStringW(section, key.c_str(), L"?", buf.data(), DWORD(buf.size()), ini_.c_str());
        buf.resize(wcslen(buf.c_str()));
        if (buf == L"?") return std::nullopt;   // '?' cannot appear in a path
        return buf;
    };
    auto number = [&](const wchar_t* key, int fallback) {
        auto s = read(L"Window", key);
        return s && !s->empty() ? _wtoi(s->c_str()) : fallback;
    };

    treeWidth_ = number(L"TreeWidth", 220);
    showHidden_ = number(L"ShowHidden", 0) != 0;
    list_.setSort(SortKey(std::clamp(number(L"SortColumn", 0), 0, 3)), number(L"SortAscending", 1) != 0);
    const int defaults[] = {320, 150, 170, 90};
    std::vector<int> widths;
    for (int i = 0; i < 4; ++i)
        widths.push_back(scale(number((L"Column" + std::to_wstring(i)).c_str(), defaults[i]), dpi_));
    list_.setColumnWidths(widths);

    auto count = read(L"Tabs", L"Count");
    int n = count ? _wtoi(count->c_str()) : 0;
    std::vector<std::wstring> paths;
    for (int i = 0; i < n && !cmd.onlyThese; ++i)   // -n: only the command line's folders
        if (auto path = read(L"Tabs", std::to_wstring(i))) paths.push_back(*path);
    auto active = read(L"Tabs", L"Active");
    int activeIndex = addStartFolders(paths, active && !cmd.onlyThese ? _wtoi(active->c_str()) : 0, cmd.folders);
    if (paths.empty()) paths.push_back(knownFolder(FOLDERID_Profile));
    for (const auto& path : paths) tabs_.push_back(makeTab(path));
    active_ = std::clamp(activeIndex, 0, int(tabs_.size()) - 1);

    int w = number(L"Width", 0), h = number(L"Height", 0);
    if (w > 0 && h > 0) {
        WINDOWPLACEMENT wp{};
        wp.length = sizeof wp;
        int x = number(L"X", 0), y = number(L"Y", 0);
        wp.rcNormalPosition = {x, y, x + w, y + h};
        wp.showCmd = number(L"Maximized", 0) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        SetWindowPlacement(wnd_, &wp);
    } else {
        ShowWindow(wnd_, useSettings_ ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);   // a self-test never steals focus
    }
}

void App::saveSettings() {
    if (!useSettings_) return;
    // Create the file as UTF-16 first, or Windows writes it in the ANSI code page
    // and non-English folder names would be lost.
    if (GetFileAttributesW(ini_.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(ini_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;   // read-only location: nothing to save to
        const wchar_t bom = 0xFEFF;
        DWORD written = 0;
        WriteFile(f, &bom, sizeof bom, &written, nullptr);
        CloseHandle(f);
    }
    auto write = [&](const wchar_t* section, const std::wstring& key, const std::wstring& value) {
        WritePrivateProfileStringW(section, key.c_str(), value.c_str(), ini_.c_str());
    };
    auto number = [&](const wchar_t* key, int value) { write(L"Window", key, std::to_wstring(value)); };

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
    number(L"ShowHidden", showHidden_ ? 1 : 0);
    number(L"SortColumn", int(list_.sortKey()));
    number(L"SortAscending", list_.sortAscending() ? 1 : 0);
    std::vector<int> widths = list_.columnWidths();
    for (size_t i = 0; i < widths.size(); ++i)
        number((L"Column" + std::to_wstring(i)).c_str(), MulDiv(widths[i], 96, int(dpi_)));

    if (!saveTabs_) return;
    WritePrivateProfileStringW(L"Tabs", nullptr, nullptr, ini_.c_str());   // drop old tabs
    write(L"Tabs", L"Count", std::to_wstring(tabs_.size()));
    write(L"Tabs", L"Active", std::to_wstring(active_));
    for (size_t i = 0; i < tabs_.size(); ++i) write(L"Tabs", std::to_wstring(i), tabs_[i].path);
}
