// The main window (app.h): what the user does in it, and how the folder loads come back to it.
#include "app.h"

#include "crashlog.h"
#include "shell.h"

#include <dbt.h>
#include <initguid.h>
#include <oleacc.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <optional>

namespace {

enum : int { ID_BACK = 100, ID_FORWARD, ID_UP, ID_ADDRESS, ID_FILTER, ID_TABS, ID_PLUS, ID_TREE, ID_LIST, ID_STATUS, ID_REFRESH };
enum : UINT {
    WM_APP_LOADED = WM_APP + 1, WM_APP_CHANGED, WM_APP_CLOSETAB, WM_APP_RENAME, WM_APP_ICONS, WM_APP_TREE,
    WM_APP_MIDDLECLICK, WM_APP_TABS_SAVED, WM_APP_TABS_READ, WM_APP_PRESSTAB, WM_APP_DRAGTAB, WM_APP_TREE_ICONS,
    WM_APP_BOOKMARKS, WM_APP_NEWWINDOW, WM_APP_BOOKMARKED, WM_APP_DELETED, WM_APP_COMMAND, WM_APP_TERMINAL
};

struct CommandDone {   // a typed command's reply from its worker (goToTyped)
    UINT seq;
    bool ran;
    std::wstring text;   // as typed
};

constexpr size_t kMaxHistory = 100;

// The newest load, for the load threads: a replaced one stops reading (4.2). Global: a thread can outlive the window.
std::atomic<UINT> g_newestLoad{0};
std::atomic<UINT> g_keptLoad{0};   // a tab switch's load a move took over: it reads on all the same (App::kept_)

constexpr COLORREF kBackground = RGB(235, 235, 235);   // soft gray: less glare than white
constexpr COLORREF kHint = RGB(160, 160, 160);         // the empty filter box's funnel: there, but quiet (5.1)
constexpr COLORREF kLine = RGB(219, 219, 219);         // the line under each row of the list: there, but quiet (5.2)

bool highContrastOn() {
    HIGHCONTRASTW hc{sizeof hc, 0, nullptr};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

bool is(const std::wstring& verb, const wchar_t* name) { return _wcsicmp(verb.c_str(), name) == 0; }
// The menu verbs that open an item, here or in a new tab (or window): Qoqdir opens folders itself.
bool isOpenVerb(const std::wstring& verb) { return is(verb, L"open") || is(verb, L"explore"); }
// opennewprocess is Explorer's "Open in new process": like a new window, so a background tab here, not an Explorer
// window (4.4).
bool isNewTabVerb(const std::wstring& verb) {
    return is(verb, L"opennewtab") || is(verb, L"opennewwindow") || is(verb, L"opennewprocess");
}

// The text cut to fit 'width' pixels on 'dc', ending in "…" if it was longer. Windows counts the two halves of an
// emoji as one character, so the cut never splits one.
std::wstring fitText(HDC dc, const std::wstring& text, int width) {
    int fit = 0;
    SIZE size{};
    GetTextExtentExPointW(dc, text.c_str(), int(text.size()), width, &fit, nullptr, &size);
    if (fit >= int(text.size())) return text;
    GetTextExtentPoint32W(dc, L"…", 1, &size);
    GetTextExtentExPointW(dc, text.c_str(), int(text.size()), width - size.cx, &fit, nullptr, &size);
    return text.substr(0, size_t(fit)) + L"…";
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

int tabAt(HWND bar, POINT screenPt) {   // the tab under a point on the screen, or -1
    TCHITTESTINFO hit{};
    hit.pt = screenPt;
    ScreenToClient(bar, &hit.pt);
    return TabCtrl_HitTest(bar, &hit);
}

// The tab bar. A press is handled here, not by the control: it makes the tab current at once and starts a drag along
// the bar that moves it. A middle-click closes a tab; a double-click on the empty part of the bar is the "+" button.
LRESULT CALLBACK tabBarProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    TCHITTESTINFO hit{};
    hit.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    switch (msg) {
    case WM_NCHITTEST:   // the control answers "not mine" off the tabs, and a click there would go to the window behind
        return HTCLIENT;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (int index = TabCtrl_HitTest(h, &hit); index >= 0) {
            if (index != TabCtrl_GetCurSel(h)) SendMessageW(GetParent(h), WM_APP_PRESSTAB, WPARAM(index), 0);
            SetCapture(h);   // the drag lasts until the button comes up, even outside the bar
        } else if (msg == WM_LBUTTONDBLCLK) {
            PostMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(ID_PLUS, BN_CLICKED), 0);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (int over = TabCtrl_HitTest(h, &hit); over >= 0 && GetCapture() == h && (wp & MK_LBUTTON))
            SendMessageW(GetParent(h), WM_APP_DRAGTAB, WPARAM(over), lp);
        break;
    case WM_LBUTTONUP:
        if (GetCapture() == h) ReleaseCapture();
        return 0;
    case WM_MBUTTONUP:
        if (int index = TabCtrl_HitTest(h, &hit); index >= 0)
            PostMessageW(GetParent(h), WM_APP_CLOSETAB, WPARAM(index), 0);
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

// While the filter box is empty, a small pale funnel at its right end says what it is, quieter than a word (5.1). It
// is the Back button's icon font's, drawn over what the box paints, and for WM_PRINTCLIENT too.
LRESULT CALLBACK filterProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    LRESULT r = DefSubclassProc(h, msg, wp, lp);
    if ((msg == WM_PAINT || msg == WM_PRINTCLIENT) && GetWindowTextLengthW(h) == 0) {
        HDC dc = msg == WM_PAINT ? GetDC(h) : reinterpret_cast<HDC>(wp);
        int saved = SaveDC(dc);
        RECT rc;
        GetClientRect(h, &rc);
        rc.right -= scale(4, GetDpiForWindow(h));
        SelectObject(dc, reinterpret_cast<HGDIOBJ>(SendMessageW(GetDlgItem(GetParent(h), ID_BACK), WM_GETFONT, 0, 0)));
        SetTextColor(dc, highContrastOn() ? GetSysColor(COLOR_GRAYTEXT) : kHint);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, L"\uE71C", 1, &rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);   // the funnel
        RestoreDC(dc, saved);
        if (msg == WM_PAINT) ReleaseDC(h, dc);
    }
    return r;
}

}  // namespace

// ---------------------------------------------------------------- setup

// What the program that started this one asked of its window, in STARTUPINFO: no focus, as a script's
// SW_SHOWNOACTIVATE asks (the command-line checks use it, 5.10: the window then opens behind whatever has the focus),
// and a place and size, as Open in new window gives (5.8), taken only when that is on a monitor.
App::StartAsk App::startAsk() {
    STARTUPINFOW si{};
    si.cb = sizeof si;
    GetStartupInfoW(&si);
    StartAsk ask;
    bool show = si.dwFlags & STARTF_USESHOWWINDOW;
    ask.quiet = show && (si.wShowWindow == SW_SHOWNOACTIVATE || si.wShowWindow == SW_SHOWMINNOACTIVE ||
                         si.wShowWindow == SW_SHOWNA);
    ask.maximized = show && si.wShowWindow == SW_SHOWMAXIMIZED;
    // Summed as DWORDs, which wrap: nonsense from the program that started this one can't trip the overflow trap.
    RECT place{LONG(si.dwX), LONG(si.dwY), LONG(si.dwX + si.dwXSize), LONG(si.dwY + si.dwYSize)};
    if ((si.dwFlags & STARTF_USEPOSITION) && (si.dwFlags & STARTF_USESIZE) &&
        MonitorFromRect(&place, MONITOR_DEFAULTTONULL))
        ask.place = StartAsk::Place{int(si.dwX), int(si.dwY), int(si.dwXSize), int(si.dwYSize)};
    return ask;
}

bool App::create(HINSTANCE instance, const CommandLine& cmd, bool useSettings) {
    useSettings_ = useSettings;
    std::wstring exe = exePath();
    // Without settings (self-test), read from a file that does not exist: every value is then a default.
    ini_ = useSettings_ ? joinPath(parentPath(exe), toLower(kAppName) + L".ini") : L"?";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));   // qoqdir.rc; Windows takes the title bar's small one from it too
    wc.lpszClassName = kAppName;
    RegisterClassExW(&wc);

    // Where to make the window: where the program that started this one asked (Open in new window, 5.8), else the
    // saved place when it is on a monitor, else the default. Made there and not moved after, Windows does not rescale
    // it (WM_DPICHANGED) and change its size, which it would for a place on a monitor of other scaling (5.11).
    StartAsk ask = startAsk();
    UINT sysDpi = GetDpiForSystem();
    StartAsk::Place at{CW_USEDEFAULT, CW_USEDEFAULT, scale(1100, sysDpi), scale(700, sysDpi)};
    if (ask.place) {
        at = *ask.place;
    } else {
        int w = iniNumber(L"Width", 0), h = iniNumber(L"Height", 0), x = iniNumber(L"X", 0), y = iniNumber(L"Y", 0);
        RECT r{x, y, x + w, y + h};   // a place on a monitor that is gone (a laptop undocked) falls back to the default
        if (w > 0 && h > 0 && MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) at = {x, y, w, h};
    }
    CreateWindowExW(0, wc.lpszClassName, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, at.x, at.y, at.width,
                    at.height, nullptr, nullptr, instance, this);
    if (!wnd_) return false;
    dpi_ = GetDpiForWindow(wnd_);

    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exStyle, int id) {
        return CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, wnd_,
                               reinterpret_cast<HMENU>(INT_PTR(id)), instance, nullptr);
    };
    back_ = make(L"BUTTON", L"\uE72B", BS_PUSHBUTTON, 0, ID_BACK);   // Explorer's arrow icons (applyFont)
    forward_ = make(L"BUTTON", L"\uE72A", BS_PUSHBUTTON, 0, ID_FORWARD);
    up_ = make(L"BUTTON", L"\uE74A", BS_PUSHBUTTON, 0, ID_UP);
    refresh_ = make(L"BUTTON", L"\uE72C", BS_PUSHBUTTON, 0, ID_REFRESH);
    address_ = make(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, ID_ADDRESS);
    filter_ = make(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, ID_FILTER);
    // Windows' plain tabs, as in its dialogs (the current one white and taller), all one width (showTabs).
    tabBar_ = make(WC_TABCONTROLW, L"", TCS_FOCUSNEVER, 0, ID_TABS);
    plus_ = make(L"BUTTON", L"+", BS_PUSHBUTTON, 0, ID_PLUS);
    // Bookmarks are shortcuts in a folder next to the exe (in a self-test: in the test folder).
    std::wstring bookmarks = joinPath(useSettings ? parentPath(exe) : cmd.folders.at(0), L"Bookmarks");
    CreateDirectoryW(bookmarks.c_str(), nullptr);   // fails harmlessly if it exists
    tree_.create(wnd_, ID_TREE, WM_APP_TREE, WM_APP_TREE_ICONS, bookmarks);
    bookmarksWatch_ = FolderWatch(wnd_, WM_APP_BOOKMARKS, bookmarks);
    list_.create(wnd_, ID_LIST);
    status_ = make(STATUSCLASSNAMEW, L"", SBARS_SIZEGRIP, 0, ID_STATUS);
    // Each button's tooltip, and the filter box's, says what it does and its key, and screen readers read the same as
    // its name (5.1).
    tips_ = CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT,
                            CW_USEDEFAULT, CW_USEDEFAULT, wnd_, nullptr, instance, nullptr);
    Com<IAccPropServices> names;   // its IID by hand: MinGW's oleacc.h gives it no __uuidof for IID_PPV_ARGS
    CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, IID_IAccPropServices,
                     reinterpret_cast<void**>(names.put()));
    const std::pair<HWND, const wchar_t*> named[] = {{back_, L"Back (Alt+Left)"}, {forward_, L"Forward (Alt+Right)"},
                                                      {up_, L"Up (Backspace)"}, {refresh_, L"Refresh (F5)"},
                                                      {filter_, L"Filter (Ctrl+F)"}};
    for (auto [h, text] : named) {
        TTTOOLINFOW ti{};
        ti.cbSize = sizeof ti;
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = wnd_;
        ti.uId = reinterpret_cast<UINT_PTR>(h);
        ti.lpszText = const_cast<wchar_t*>(text);
        SendMessageW(tips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        if (names) names->SetHwndPropStr(h, OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_NAME, text);
    }

    SHAutoComplete(address_, SHACF_FILESYS_DIRS);
    SetWindowSubclass(tabBar_, tabBarProc, 1, 0);
    SetWindowSubclass(filter_, filterProc, 1, 0);
    SetWindowSubclass(list_.hwnd(), middleClickProc, 1, 0);
    SetWindowSubclass(tree_.hwnd(), middleClickProc, 1, 0);
    // Drops land in the folder row under the pointer (or a shortcut's folder), which lights up, or else in the folder
    // shown; never in This PC. Our own drag takes neither the folder it comes from nor a folder it carries (5.8).
    listDrop_ = newDropTarget(wnd_, [this](const POINT* pt) -> std::optional<std::wstring> {
        std::optional<std::wstring> row = list_.dropFolder(pt, draggingFiles_);
        if (!pt || list_.dir().empty()) return std::nullopt;
        if (row || draggingFiles_) return row;
        return list_.dir();
    });
    RegisterDragDrop(list_.hwnd(), listDrop_.get());
    // Dropping on a tab puts the files in that tab's folder.
    tabDrop_ = newDropTarget(wnd_, [this](const POINT* pt) { return tabDropTarget(pt); });
    RegisterDragDrop(tabBar_, tabDrop_.get());
    // And on a folder in the tree, in that folder; not back where they come from.
    treeDrop_ = newDropTarget(wnd_, [this](const POINT* pt) -> std::optional<std::wstring> {
        std::optional<std::wstring> dir = tree_.dropTarget(pt);
        bool fromHere = dir && draggingFiles_ && samePath(*dir, dragSource_);
        std::wstring over;   // a node that refuses says why in the status bar (5.5)
        if ((!dir || fromHere) && pt && tree_.pathAt(*pt, &over))
            setMessage(fromHere ? L"The files come from there" : L"Only a folder on this PC takes a drop");
        return fromHere ? std::nullopt : dir;
    });
    RegisterDragDrop(tree_.hwnd(), treeDrop_.get());

    applyColors(highContrastOn());
    applyFont();
    tree_.fill();

    loadSettings(cmd, ask);
    layout();
    showTab();
    if (!ask.quiet) focusList();   // giving a control the focus activates the window, which a quiet start may not
    watchForHangs(wnd_);
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
    LOGFONTW icons = ncm.lfMessageFont;   // Back, Forward, Up and Refresh: Explorer's icons, as tall as the text
    lstrcpynW(icons.lfFaceName, L"Segoe MDL2 Assets", LF_FACESIZE);
    Font arrows(CreateFontIndirectW(&icons));
    for (HWND h : {address_, filter_, tabBar_, plus_, tree_.hwnd(), list_.hwnd(), status_})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font.get()), TRUE);
    for (HWND h : {back_, forward_, up_, refresh_}) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(arrows.get()), TRUE);
    font_ = std::move(font);   // the old fonts are deleted only after no control uses them
    arrowFont_ = std::move(arrows);

    HDC dc = GetDC(wnd_);
    HGDIOBJ old = SelectObject(dc, font_.get());
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(wnd_, dc);
    rowHeight_ = tm.tmHeight + scale(10, dpi_);
    tabHeight_ = tm.tmHeight + scale(12, dpi_);   // tab text plus the control's own padding
}

// The soft gray, or a high-contrast theme's own colours: the theme's text could be white, unreadable on the gray.
void App::applyColors(bool highContrast) {
    static HBRUSH gray = CreateSolidBrush(kBackground);   // lives as long as the process
    COLORREF color = highContrast ? GetSysColor(COLOR_WINDOW) : kBackground;
    ListView_SetBkColor(list_.hwnd(), color);
    ListView_SetTextBkColor(list_.hwnd(), color);
    list_.setLineColor(highContrast ? GetSysColor(COLOR_GRAYTEXT) : kLine);
    TreeView_SetBkColor(tree_.hwnd(), color);
    SetClassLongPtrW(wnd_, GCLP_HBRBACKGROUND, LONG_PTR(highContrast ? GetSysColorBrush(COLOR_WINDOW) : gray));
    InvalidateRect(wnd_, nullptr, TRUE);
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
    int filterW = std::min(scale(120, dpi_), w / 3);
    int y = pad, x = pad;
    HDWP d = BeginDeferWindowPos(11);
    auto place = [&](HWND hwnd, int px, int py, int pw, int ph) {
        if (d) d = DeferWindowPos(d, hwnd, nullptr, px, py, std::max(pw, 0), std::max(ph, 0), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    for (HWND b : {back_, forward_, up_, refresh_}) {
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
    if (dragging_) treeWidth_ = unscale(treeW, dpi_);   // a drag past the edge keeps the width shown, not one past it
    int bodyH = h - statusH - y;
    place(tree_.hwnd(), 0, y, treeW, bodyH);
    place(list_.hwnd(), treeW + splitW, y, w - treeW - splitW, bodyH);
    splitterX_ = treeW;
    RECT bar{};
    GetClientRect(tabBar_, &bar);
    if (d) EndDeferWindowPos(d);
    if (bar.right != std::max(w - btn - pad, 0)) showTabs();   // the tabs share a bar of a new width
}

// ---------------------------------------------------------------- messages

LRESULT App::onMessage(UINT msg, WPARAM wp, LPARAM lp) {
    // These carry a heap object made on a worker in lParam. Another program broadcasting one of their numbers (apps
    // do broadcast in the WM_APP range) would hand a null or stray pointer; ignore one with no pointer (4.10).
    if (!lp && (msg == WM_APP_LOADED || msg == WM_APP_ICONS || msg == WM_APP_TREE || msg == WM_APP_TREE_ICONS ||
                msg == WM_APP_TABS_READ || msg == WM_APP_DELETED || msg == WM_APP_COMMAND))
        return 0;
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
        showTabs();   // and the tabs' width and names
        return 0;
    }
    case WM_SYSCOLORCHANGE:   // e.g. a high-contrast theme came or went: the controls hear of it only from here
        for (HWND h : {back_, forward_, up_, refresh_, address_, filter_, tabBar_, plus_, tree_.hwnd(), list_.hwnd(), status_})
            SendMessageW(h, WM_SYSCOLORCHANGE, wp, lp);
        applyColors(highContrastOn());
        return 0;
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
            // The funnel shows only in the empty box, which repaints just its text as that changes: all of it when it
            // empties or fills.
            if (bool empty = GetWindowTextLengthW(filter_) == 0; std::exchange(filterEmpty_, empty) != empty)
                InvalidateRect(filter_, nullptr, TRUE);
            return 0;
        }
        if (code != BN_CLICKED) return 0;
        if (id == ID_BACK) goBack();
        if (id == ID_FORWARD) goForward();
        if (id == ID_UP) goUp();
        if (id == ID_REFRESH) refreshAll();
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
        onFolderChanged();
        return 0;
    case WM_APP_BOOKMARKS:   // a shortcut or folder came or went in Bookmarks: a drop on its node, or another program
        acknowledgeChange(wp, lp);
        tree_.refreshFolder(tree_.bookmarks());
        return 0;
    case WM_APP_ICONS: {   // the shown folder's own icons
        std::unique_ptr<IconBatch> batch(reinterpret_cast<IconBatch*>(lp));
        list_.setIcons(*batch);
        return 0;
    }
    case WM_APP_TREE_ICONS: {   // bookmark shortcuts get their own icons the same way
        std::unique_ptr<IconBatch> batch(reinterpret_cast<IconBatch*>(lp));
        tree_.setIcons(reinterpret_cast<HTREEITEM>(wp), *batch);
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
        if (wp) setMessage(L"Tabs saved in Bookmarks\\Tabs, replacing the earlier set");
        else warn(L"Can't save every tab in Bookmarks\\Tabs");
        return 0;
    case WM_APP_NEWWINDOW:   // the new window did not start (openInNewWindow)
        warn(L"Can't open a new window: " + errorText(DWORD(wp)));
        return 0;
    case WM_APP_BOOKMARKED:   // the shortcut was written on a worker (bookmarkTab)
        tree_.refreshFolder(tree_.bookmarks());
        if (wp) setMessage(L"Bookmarked");
        else warn(L"Can't make the bookmark");
        return 0;
    case WM_APP_DELETED: {   // the recycle finished on a worker (deleteSelection); select what followed (4.6)
        std::unique_ptr<ViewState> v(reinterpret_cast<ViewState*>(lp));
        if (wp && v) refresh(*v);   // nothing deleted (No, or it failed): the selection was already cleared
        return 0;
    }
    case WM_APP_TERMINAL:   // Ctrl+`'s terminal was looked for and started on a worker (openTerminal)
        if (!wp) warn(L"Can't start a terminal");
        return 0;
    case WM_APP_COMMAND: {   // a typed command was looked up and started on a worker (goToTyped)
        std::unique_ptr<CommandDone> c(reinterpret_cast<CommandDone*>(lp));
        ++commandsDone_;
        if (c->seq != commands_) return 0;   // one typed since decides what the address bar shows
        bool untouched = windowText(address_) == c->text;
        if (!c->ran) {
            warn(L"Can't find \"" + c->text + L"\"");
            if (untouched) focusEdit(address_);   // left selected, like a bad path, so it can be corrected (5.3)
        } else if (untouched) {
            SetWindowTextW(address_, locationText(tab().path).c_str());
            if (GetFocus() == address_) focusList();
        }
        return 0;
    }
    case WM_APP_TABS_READ: {
        std::unique_ptr<std::vector<std::wstring>> folders(reinterpret_cast<std::vector<std::wstring>*>(lp));
        addSavedTabs(*folders);
        return 0;
    }
    case WM_APP_CLOSETAB:
        closeTab(int(wp));
        return 0;
    case WM_APP_PRESSTAB:
        switchTab(int(wp));
        return 0;
    case WM_APP_DRAGTAB:
        dragTab(int(wp), GET_X_LPARAM(lp));
        return 0;
    case WM_MOUSEWHEEL: {   // over the tab bar: switches tabs, up to the left (a touchpad's small steps add up)
        RECT bar{};
        GetWindowRect(tabBar_, &bar);
        if (!PtInRect(&bar, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) break;
        wheel_ += GET_WHEEL_DELTA_WPARAM(wp);
        int steps = wheel_ / WHEEL_DELTA;
        wheel_ %= WHEEL_DELTA;
        if (steps) switchTab(std::clamp(active_ - steps, 0, int(tabs_.size()) - 1));
        return 0;
    }
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
        case APPCOMMAND_BROWSER_REFRESH: refreshAll(); return TRUE;
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
            treeWidth_ = unscale(std::max(GET_X_LPARAM(lp), 0), dpi_);
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
    case WM_CLOSE: {
        // A window going with many tabs offers to save them, so a set isn't lost by one click (5.7). Saved here and
        // now: a worker would die with the window. A self-test never asks.
        if (asking_) return 0;   // a second close while the question is up (a script's WinClose) would save twice
        if (tabs_.size() >= 4 && !selfTest_) {
            asking_ = true;
            int answer = MessageBoxW(wnd_, L"Save these tabs in Bookmarks\\Tabs before closing?", kAppName,
                                     MB_YESNOCANCEL | MB_ICONQUESTION);
            asking_ = false;
            if (answer == IDCANCEL) return 0;
            if (answer == IDYES) saveFolderShortcuts(joinPath(tree_.bookmarks(), L"Tabs"), tabFolders());
        }
        saveSettings();
        DestroyWindow(wnd_);
        return 0;
    }
    case WM_DESTROY:
        watch_ = {};
        bookmarksWatch_ = {};
        RevokeDragDrop(list_.hwnd());
        RevokeDragDrop(tabBar_);
        RevokeDragDrop(tree_.hwnd());
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd_, msg, wp, lp);
}

// At most one refresh per interval; it waits while a rename or a load is in progress (onTimer).
void App::queueRefresh() {
    refreshForWatch_ = false;
    if (refreshQueued_) return;
    refreshQueued_ = true;
    SetTimer(wnd_, kRefreshTimer, 400, nullptr);
}

// A refresh only changes in the folder shown asked for, from its watch or seen by its read, is about that folder: it
// goes with the folder's watch (onLoaded).
void App::onFolderChanged() {
    bool alone = !refreshQueued_ || refreshForWatch_;
    queueRefresh();
    refreshForWatch_ = alone;
}

void App::onTimer(UINT_PTR id) {
    if (id == kRefreshTimer) {
        // Try again later: never disturb a rename, and never cancel a folder that is loading.
        if (list_.renaming() || loading()) return;
        KillTimer(wnd_, kRefreshTimer);
        refreshQueued_ = false;
        // What a delete, rename or new folder asked to select, if its folder is still shown. Not available: F5 (4.3).
        QueuedView q = std::exchange(queuedView_, {});
        if (!tab().unavailable) refresh(list_.shows(q.dir) ? std::move(q.view) : ViewState{});
    } else if (id == kStatusTimer) {
        KillTimer(wnd_, kStatusTimer);
        showStatus();
    } else if (id == kLoadingTimer) {
        KillTimer(wnd_, kLoadingTimer);
        if (loading()) {   // say "Loading..." only if it is slow
            // A tab waiting for its switch's read after a failed move gets back what this takes the place of
            // (onLoaded, 4.2): the move's error, or a message shown since.
            if (!pending_.generation) keptSaid_ = message();
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
    } else if (bool shortcut = false; from == tree_.hwnd() && tree_.pathAt(screenPt, &path, &shortcut)) {
        openNode(path, true, shortcut);
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
                dragSource_ = list_.dir();
                draggingFiles_ = true;
                dragOut(wnd_, dragSource_, items);
                draggingFiles_ = false;
            }
            break;
        }
        }
        return 0;
    }
    if (nm->hwndFrom == tree_.hwnd()) {
        std::wstring path;
        bool shortcut = false;
        if (tree_.onNotify(nm, &path, &shortcut)) openNode(path, false, shortcut);
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
        const MenuItems m = menuItems();
        if (m.rows.empty()) {
            const std::wstring& dir = m.dir;
            if (!dir.empty()) {
                bool copied = canPaste();   // else Paste is greyed
                auto paste = [&](bool shortcuts) { if (list_.shows(dir)) pasteInto(wnd_, dir, shortcuts); };
                showMenu(wnd_, backgroundMenu(wnd_, dir).get(), pt, nullptr,
                         {{L"Paste\tCtrl+V", copied, [&] { paste(false); }},
                          {L"Paste shortcut", copied, [&] { paste(true); }},
                          {L"Open in new window", true, [&] { openInNewWindow(dir); }},
                          {L"Open in Explorer", true, [&] { openInExplorer(wnd_, dir, false); }},
                          {L"Search in Explorer", true, [&] { openInExplorer(wnd_, dir, true); }}});
            }
            return;
        }
        showMenu(wnd_, itemMenu(wnd_, m.dir, list_.selectedItems()).get(), pt,
                 [&](const std::wstring& verb) { return itemVerb(verb, m); });
        return;
    }
    if (from == tree_.hwnd()) {
        std::wstring path;
        bool shortcut = false;
        if (!tree_.pathAt(pt, &path, &shortcut)) return;
        if (keyboard) {
            RECT r{};
            GetWindowRect(tree_.hwnd(), &r);
            pt = {r.left + scale(24, dpi_), r.top + scale(24, dpi_)};
        }
        showMenu(wnd_, itemMenu(wnd_, path).get(), pt,
                 [&](const std::wstring& verb) { return nodeVerb(verb, path, shortcut); });
        tree_.refreshFolder(parentPath(path));   // a folder deleted from its own menu leaves the tree
    }
}

App::MenuItems App::menuItems() const {
    MenuItems m{list_.dir(), list_.selectedNames(), list_.selection()};
    // With a folder or shortcut among them, activate opens them all (folders here, files with Windows).
    m.opensHere = std::any_of(m.rows.begin(), m.rows.end(), [&](int r) {
        return list_.at(r).isDir() || isShortcut(list_.at(r).name);
    });
    return m;
}

bool App::itemVerb(const std::wstring& verb, const MenuItems& m) {
    bool open = isOpenVerb(verb), inNewTab = isNewTabVerb(verb), location = is(verb, L"opencontaining");
    if (!is(verb, L"rename") && !is(verb, L"delete") && !location && !(m.opensHere && (open || inNewTab))) return false;
    if (!list_.shows(m.dir) || list_.selectedNames() != m.names) return true;   // changed: do nothing
    int row = list_.focused() >= 0 ? list_.focused() : m.rows[0];
    if (is(verb, L"rename")) list_.rename(row);
    else if (is(verb, L"delete")) deleteSelection(GetKeyState(VK_SHIFT) < 0);   // like the Delete key (4.6)
    else if (location) openTargetLocation(list_.pathAt(row));                   // a shortcut's Open file location
    else activate(inNewTab);
    return true;
}

bool App::nodeVerb(const std::wstring& verb, const std::wstring& path, bool shortcut) {
    if (isOpenVerb(verb)) openNode(path, false, shortcut);
    else if (isNewTabVerb(verb)) openNode(path, true, shortcut);
    else if (is(verb, L"rename")) renameNode(path);
    else if (is(verb, L"opencontaining")) openTargetLocation(path);   // a bookmark's Open file location
    else return false;
    return true;
}

void App::onTabMenu() {
    POINT screen{};
    GetCursorPos(&screen);
    int index = tabAt(tabBar_, screen);
    if (index < 0) return;
    Menu menu(CreatePopupMenu());
    AppendMenuW(menu.get(), MF_STRING, 1, L"Duplicate tab");
    AppendMenuW(menu.get(), MF_STRING, 2, L"Close tab\tCtrl+W");
    AppendMenuW(menu.get(), MF_STRING | (tabs_.size() > 1 ? 0 : MF_GRAYED), 3, L"Close other tabs");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, 6, L"Bookmark this folder");
    AppendMenuW(menu.get(), MF_STRING, 4, L"Save tabs\tCtrl+S");
    AppendMenuW(menu.get(), MF_STRING, 5, L"Open saved tabs\tCtrl+O");
    int cmd = TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, wnd_, nullptr);
    if (cmd == 1) newTab(tabs_[size_t(index)].path, true);
    if (cmd == 2) closeTab(index);
    if (cmd == 3) closeOtherTabs(index);
    if (cmd == 4) saveTabs();
    if (cmd == 5) openSavedTabs();
    if (cmd == 6) bookmarkTab(index);
}

// ---------------------------------------------------------------- keyboard

bool App::handleKey(const MSG& m) {
    if (m.message != WM_KEYDOWN && m.message != WM_SYSKEYDOWN && m.message != WM_CHAR) return false;
    // Only our own window: never steal keys from dialogs such as Properties.
    if (!wnd_ || (m.hwnd != wnd_ && !IsChild(wnd_, m.hwnd))) return false;
    // A key counts where it was typed: an input method's word arrives as several characters at once, and the first
    // moves the focus to the filter box before the others are read.
    return onKey({m.message, m.wParam, m.hwnd, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0,
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
            hint(hintedFilter_, L"Typing filters the list; Esc shows all again");
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
    if (ctrl && shift && (k == VK_PRIOR || k == VK_NEXT)) { moveTab(active_ + (k == VK_NEXT ? 1 : -1)); return true; }
    if ((ctrl && k == 'L') || (alt && k == 'D') || (k == VK_F4 && !alt)) { focusEdit(address_); return true; }
    if ((ctrl && k == 'F') || k == VK_F3) { focusEdit(filter_); return true; }
    if (k == VK_F5 || (ctrl && k == 'R')) { refreshAll(); return true; }
    if (ctrl && k == VK_OEM_3) { openTerminal(); return true; }   // Ctrl+`, the key left of 1
    if (k == VK_F1) { showKeys(wnd_); return true; }
    if (ctrl && k == 'H') {   // hidden items are always read: the list just shows or hides them
        list_.setShowHidden(!list_.showHidden());
        setMessage(list_.showHidden() ? L"Showing hidden files" : L"Hiding hidden files");
        return true;
    }
    if (alt && k == VK_LEFT) { goBack(); return true; }
    if (alt && k == VK_RIGHT) { goForward(); return true; }
    if (alt && k == VK_UP) { goUp(); return true; }
    if (alt && k == VK_RETURN && focus == list) {   // the selection's properties, or with nothing selected the folder's own
        // A tab not available (4.3) or still switching shows a cleared list, which shows no folder, not This PC.
        if (!list_.shows(tab().path)) return true;
        std::vector<Item> items = list_.selectedItems();
        Com<IContextMenu> menu = items.empty() ? itemMenu(wnd_, list_.dir()) : itemMenu(wnd_, list_.dir(), items);
        invokeVerb(wnd_, menu.get(), "properties");
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
        if (ctrl && k == VK_BACK) { deleteWordBefore(address_); return true; }
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
    if (k == VK_BACK && (focus == list || focus == tree_.hwnd())) {
        if (goUp()) hint(hintedUp_, L"Up (Alt+Left goes back)", true);
        return true;
    }
    if (focus == list) {
        if (k == VK_RETURN) { activate(ctrl); return true; }
        if (k == VK_DELETE) { deleteSelection(shift); return true; }
        if (k == VK_F2) {
            std::vector<int> rows = list_.selection();
            if (!rows.empty()) list_.rename(list_.focused() >= 0 && !ctrl ? list_.focused() : rows[0]);
            return true;
        }
        if (k == VK_ESCAPE && GetWindowTextLengthW(filter_) > 0) { SetWindowTextW(filter_, L""); return true; }
        if (ctrl && shift && k == 'N') { newFolder(); return true; }
        if (ctrl && k == 'A') { list_.selectAll(); return true; }
        if (ctrl && shift && k == 'C') { copyPaths(); return true; }
        if (ctrl && (k == 'C' || k == 'X')) {
            std::vector<Item> items = list_.selectedItems();
            if (items.empty()) return true;
            if (copyToClipboard(wnd_, list_.dir(), items, k == 'X')) {
                setMessage(std::to_wstring(items.size()) + (k == 'X' ? L" item(s) cut" : L" item(s) copied"));
            } else {
                warn(L"Can't put the items on the clipboard");
            }
            return true;
        }
        if (ctrl && k == 'V') { pasteInto(wnd_, list_.dir()); return true; }
        return false;
    }
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
    // A move that takes over from a tab switch lets the switch's read go on (kept_, 4.2), and so does a further move
    // on that tab; anything else makes it of no use. Set before the newest load is, so that read never stops.
    bool move = nav == Nav::New || nav == Nav::Back || nav == Nav::Forward;
    bool replacing = move && pending_.generation && pending_.tabId == tab().id;   // this tab's load still out
    // A refresh of the tab's folder that a move replaces goes with it: if the move fails, it is asked for again (4.5).
    bool refreshLost = replacing && (pending_.nav == Nav::Refresh || pending_.refreshLost);
    if (replacing && pending_.nav == Nav::Switch) {
        kept_ = std::move(pending_);
    } else if (!move) {
        kept_ = {};
    }
    g_keptLoad = kept_.generation;
    UINT generation = ++generation_;
    g_newestLoad = generation;
    pending_ = {generation, nav, std::move(view), tab().id, std::move(origin), std::move(filter), refreshLost};
    SetTimer(wnd_, kLoadingTimer, 250, nullptr);   // say "Loading..." only if it is slow
    if (nav == Nav::Switch) {
        // Show the new tab at once; until its folder loads the list has none, so nothing acts on the old one.
        list_.clear();
        showLocation();
        showStatus();
    }
    inBackground([wnd = wnd_, generation, path] {
        auto replaced = [generation] { return g_newestLoad != generation && g_keptLoad != generation; };
        auto result = std::make_unique<Listing>(listFolder(path, true, replaced));
        if (!replaced()) fillTypes(result->items);   // the shell's type names and icons, off the window's thread
        std::vector<std::wstring> own;
        for (const Item& it : result->items)
            if (hasOwnIcon(it)) own.push_back(it.name);
        // Asked before the listing goes: once it is shown, a switch's read a move took over looks replaced (kept_).
        bool icons = !own.empty() && !replaced();
        post(wnd, WM_APP_LOADED, generation, std::move(result));
        // Then the icons that need a disk (or network) lookup, so they never hold up the list.
        if (icons) post(wnd, WM_APP_ICONS, 0, std::make_unique<IconBatch>(readOwnIcons(path, own)));
    });
}

void App::onLoaded(Listing&& listing, UINT generation) {
    // A tab switch's read that a move took over (kept_) shows the tab's folder, as the switch would have, as soon as
    // it is back: while the move still loads, which then goes on from that folder, or after it came to nothing (4.2).
    bool keptBack = kept_.generation && generation == kept_.generation;
    bool counted = generation != uncountedLoad_;
    if (counted) ++loadsDone_;
    if (!keptBack && generation != pending_.generation) {   // superseded (open() sets both together)
        if (counted && listing.error != ERROR_OPERATION_ABORTED) ++loadsWasted_;
        return;
    }
    bool moving = keptBack && pending_.generation;   // the move is still out: "Loading..." and its hint are its own
    if (!moving) {
        KillTimer(wnd_, kLoadingTimer);
        // Any load, not only navigation, may have said "Loading...". What it took the place of comes back for a tab
        // that waited for its switch's read after a failed move (4.2); otherwise it goes.
        if (loadMessageShown_) setMessage(keptBack ? keptSaid_ : L"");
    }
    Pending p = std::exchange(keptBack ? kept_ : pending_, {});
    if (keptBack) g_keptLoad = 0;
    if (tab().id != p.tabId) return;
    if (p.nav == Nav::Refresh && list_.renaming()) {
        // 4.2: never apply a refresh under an open rename box (Enter would rename whatever row moved there).
        // Retry once it closes, keeping what this load was asked to select unless a newer request waits.
        if (p.view.asks() && !queuedView_.view.asks()) queuedView_ = {list_.dir(), std::move(p.view)};
        queueRefresh();
        return;
    }
    // A move's hint (5.3), said once it lands; an error takes its place. A move still out keeps its own.
    const wchar_t* hint = moving ? nullptr : std::exchange(lateHint_, nullptr);
    Tab& t = tab();

    if (listing.error) return onLoadFailed(listing, p);
    kept_ = {};   // the tab shows a folder: a switch's read kept for a move is of no use any more
    g_keptLoad = 0;

    std::wstring old = t.path;
    bool sameFolder = list_.shows(listing.path);
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
    bool reload = p.nav == Nav::Refresh && sameFolder;
    if (reload) {
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
    // The watch stays while it watches the folder shown: setting one up costs 25-75 ms, and a reload is not a change.
    // Otherwise the old one goes first, and with it a refresh only it asked for: that was about a folder no longer
    // shown, unless this one is the folder it was in, which it watched too (4.5).
    if (!watch_.watches(t.path)) {
        bool above = watch_.watchesChildOf(t.path);
        watch_ = {};
        if (refreshQueued_ && refreshForWatch_ && !above) {
            KillTimer(wnd_, kRefreshTimer);
            refreshQueued_ = false;
        }
        watch_ = FolderWatch(wnd_, WM_APP_CHANGED, t.path);
    }
    FILETIME written = listing.written;
    // The tree follows the folder shown: its node is read again when the subfolders or shortcuts changed (5.5).
    std::vector<std::wstring> treeNames = reload ? list_.treeNames() : std::vector<std::wstring>{};
    list_.setListing(std::move(listing));
    if (reload && treeNames != list_.treeNames()) tree_.refreshFolder(t.path);
    int row = list_.select(view.select, view.focus);
    if (row < 0 && !requested && view.focusRow >= 0 && list_.count() > 0)   // the focused item is gone (4.5):
        row = list_.focusRow(std::min(view.focusRow, list_.count() - 1));   // the item in its place takes the focus
    if (view.top >= 0) list_.scrollToRow(view.top);   // a refresh keeps the scroll position (4.5)...
    if (view.top < 0 || requested) list_.ensureVisible(row);   // ...but what the user just made or renamed shows
    if (view.rename && row >= 0) list_.rename(row);

    // A change during the read, before the watch was there, is missed by both: the folder's own time tells. That
    // refresh is about this folder, as the watch's would be, so it goes if another one is shown first (4.5).
    FILETIME now = folderWritten(t.path);
    if (CompareFileTime(&now, &written) != 0) onFolderChanged();
    // A move shows where the tab is now. A tab switch showed it when it began, and the address bar may hold what the
    // user typed since (5.3), unless it landed elsewhere: a tab made on a file shows the file's folder (5.10).
    if (moved) {
        showLocation(samePath(old, t.path));
        setMessage(hint ? hint : L"");   // a navigation clears the message, or says its hint
    } else if (!samePath(old, t.path)) {
        showLocation();
    }
    // The first folder shown says how to see the keys (5.10), unless the status bar says something else by now.
    if (std::exchange(firstFolder_, false) && message().empty()) setMessage(L"F1 shows the keys");
    showStatus();
}

// The address bar, title, buttons and tabs follow the current tab's folder: shown by a tab switch at once, and by a
// move once its folder is there. A load that lands or fails otherwise changes none of them.
void App::showLocation(bool sameFolder) {
    const Tab& t = tab();
    if (!sameFolder || GetFocus() != address_) SetWindowTextW(address_, locationText(t.path).c_str());
    SetWindowTextW(wnd_, displayName(t.path).c_str());   // the folder's name alone, as Explorer: the icon says which app
    EnableWindow(back_, !t.back.empty());
    EnableWindow(forward_, !t.forward.empty());
    EnableWindow(up_, !t.path.empty());
    showTabs();
}

// The folder could not be read: decide what to show or do instead.
void App::onLoadFailed(const Listing& listing, const Pending& p) {
    if (p.nav == Nav::Refresh || p.nav == Nav::Switch) {
        // A tab made on a file (qoqdir C:\src\main.cpp) shows the file's folder with the file selected, as explorer
        // /select does (5.10). The switch's read gives the tab that folder, with no history entry for the file.
        if (listing.isFile && samePath(tab().path, listing.path) && !parentPath(listing.path).empty()) {
            open(parentPath(listing.path), Nav::Switch, ViewState::selecting(leafName(listing.path)));
            return;
        }
        showUnavailable(listing.path, listing.error);
        return;
    }
    // A move that fails or opens a file leaves the tab where it was: its filter comes back, unless retyped.
    if (GetWindowTextLengthW(filter_) == 0) SetWindowTextW(filter_, p.filter.c_str());
    // A tab switch this move took over reads on: the tab waits for what it finds (onLoaded, 4.2).
    if (kept_.generation) SetTimer(wnd_, kLoadingTimer, 250, nullptr);   // "Loading..." if it is slow (4.1)
    // The refresh of the tab's folder this move replaced (open, 4.5), about that folder as the watch's would be: it
    // goes if another one is shown first.
    if (p.refreshLost) onFolderChanged();
    if (!p.origin.shortcut.empty() && listing.isFile) {
        // A shortcut that leads to a file: Windows opens it (never an unreachable one: that would hang, then fail).
        openFile(wnd_, p.origin.shortcut);
    } else if (p.origin.typed && listing.isFile) {   // a file or shortcut typed in the address bar
        if (isShortcut(listing.path)) {
            openShortcut(listing.path, false);   // a folder opens here; a program or web link is Windows' (4.4)
        } else {
            openFile(wnd_, listing.path);
            showLocation(true);   // the tab stays: what the user types there meanwhile stays too (5.3)
        }
    } else {
        // Let the user fix what they typed (5.3), unless they moved on: a dead share fails a minute later.
        if (p.origin.typed && GetFocus() == list_.hwnd()) focusEdit(address_);
        // A history entry whose folder is gone is dropped, so Back/Forward can move past it.
        Tab& t = tab();
        std::vector<std::wstring>& history = p.nav == Nav::Back ? t.back : t.forward;
        if ((p.nav == Nav::Back || p.nav == Nav::Forward) && !history.empty() &&
            samePath(history.back(), listing.path)) {
            history.pop_back();
            EnableWindow(back_, !t.back.empty());
            EnableWindow(forward_, !t.forward.empty());
        }
        warn(displayName(listing.path) + L": " + errorText(listing.error));
    }
}

// A tab never changes its folder by itself (a drive may come back): show it empty and say why. The list gets
// no folder, so nothing asks Windows about it (that can freeze the window for 20-40 s).
void App::showUnavailable(const std::wstring& path, DWORD error) {
    list_.clear();
    tab().unavailable = true;
    watch_ = {};
    setMessage(L"\"" + locationText(path) + L"\" is not available: " + errorText(error) + L"   (F5 tries again)");
    loadMessageShown_ = true;
    showStatus();
}

void App::goBack() {
    if (!tab().back.empty()) open(tab().back.back(), Nav::Back);
}

void App::goForward() {
    if (!tab().forward.empty()) open(tab().forward.back(), Nav::Forward);
}

bool App::goUp() {
    if (tab().path.empty()) return false;
    open(parentPath(tab().path), Nav::New);
    return true;
}

bool App::refresh(ViewState view) {
    // A load is in progress: start nothing (a held F5 would pile up stuck threads on a dead share). That load
    // may predate the caller's change, so keep what it wants selected for a queued refresh.
    if (loading()) {
        if (view.asks()) {
            queuedView_ = {list_.dir(), std::move(view)};
            queueRefresh();
        }
        return false;
    }
    if (view.asks()) queuedView_ = {};   // newer than any request still waiting
    open(tab().path, Nav::Refresh, std::move(view));
    return true;
}

void App::goToTyped() {
    std::wstring text = windowText(address_);
    std::wstring typed = expandVars(text);   // only what the user typed has its %VARS% expanded (a command's too)
    std::wstring path = normalizePath(typed);
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
    if (relative && tab().path.empty()) {
        warn(L"Can't find \"" + text + L"\"");
        focusEdit(address_);   // left selected, like a bad path, so it can be corrected (5.3)
        return;
    }
    // Finding the program reads every folder on the PATH, and starting it can wait on Windows (a UAC prompt): both on
    // a worker, so a folder on the PATH that doesn't answer can't hold the window (4.1). Its reply says Can't find, or
    // puts the path back and goes to the list (WM_APP_COMMAND).
    inBackground([wnd = wnd_, seq = ++commands_, typed, text, dir = list_.dir()] {
        bool ran = runCommand(wnd, typed, dir);
        post(wnd, WM_APP_COMMAND, 0, std::make_unique<CommandDone>(CommandDone{seq, ran, text}));
    });
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
        if (!folder) {
            std::optional<std::wstring> target = shortcutTarget(path, &folder);
            if (folder && opensInTab(target)) path = *target;
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
    std::optional<std::wstring> target = shortcutTarget(path, &folder);
    if (!opensInTab(target)) {
        openFile(wnd_, path);   // a web link, a program, a library...
        return;
    }
    if (inNewTab && folder) newTab(*target, false);
    else open(*target, Nav::New, {}, Origin::fromShortcut(path));
}

// A shortcut's Open file location (the menu's opencontaining): the folder its target is in, shown here with the target
// selected, where Windows would show it in Explorer (4.4).
void App::openTargetLocation(const std::wstring& lnk) {
    bool folder = false;
    std::optional<std::wstring> target = shortcutTarget(lnk, &folder);
    if (!opensInTab(target)) warn(L"Can't show where \"" + leafName(lnk) + L"\" points");   // a web link, a library...
    else if (target->empty()) open(L"", Nav::New);   // This PC: in no folder here
    else open(parentPath(*target), Nav::New, ViewState::selecting(leafName(*target)));
}

// Rename on a tree node's menu happens in the file list, in the node's parent folder.
void App::renameNode(const std::wstring& path) {
    if (parentPath(path).empty()) return;   // This PC, a drive or a share: nothing to rename here
    ViewState v = ViewState::selecting(leafName(path));
    v.rename = true;
    open(parentPath(path), Nav::New, v);
}

// A node in the tree: a folder, or a bookmark shortcut (the tree says which, so a folder named "x.lnk" opens as a
// folder, not through the shell as a link, 5.6).
void App::openNode(const std::wstring& path, bool inNewTab, bool shortcut) {
    if (shortcut) openShortcut(path, inNewTab);
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
    if (loading() && pending_.nav == Nav::Switch) return;
    SetWindowTextW(filter_, L"");   // the tab comes back unfiltered (5.4): remember the unfiltered rows
    if (list_.shows(tab().path)) tab().view = currentView();
}

// ---------------------------------------------------------------- tabs

Tab App::makeTab(const std::wstring& path) {
    Tab t;
    t.id = nextTabId_++;
    t.path = path;
    return t;
}

// New tabs go at the end, so the newest is always the last one. Switching to a tab shows the tabs anew by itself
// (open, showLocation), so only the tab functions that don't switch show them.
void App::newTab(std::wstring path, bool switchTo) {
    tabs_.push_back(makeTab(path));
    if (switchTo) switchTab(int(tabs_.size()) - 1);
    else showTabs();
}

void App::closeTab(int index) {
    if (tabs_.size() <= 1 || index < 0 || index >= int(tabs_.size())) return;
    bool wasActive = index == active_;
    tabs_.erase(tabs_.begin() + index);
    if (index < active_) --active_;
    active_ = std::min(active_, int(tabs_.size()) - 1);
    if (wasActive) showTab();
    else showTabs();
}

void App::closeOtherTabs(int index) {
    if (index < 0 || index >= int(tabs_.size())) return;
    bool wasActive = index == active_;
    Tab keep = std::move(tabs_[size_t(index)]);
    tabs_.clear();
    tabs_.push_back(std::move(keep));
    active_ = 0;
    if (wasActive) showTabs();
    else showTab();
}

void App::switchTab(int index) {
    if (index < 0 || index >= int(tabs_.size()) || index == active_) {
        showTabs();
        return;
    }
    // The tab bar never takes the focus, so an open rename box would be cancelled and the typed name lost. Commit it
    // first by moving the focus to the list, as a click anywhere else does (5.4).
    if (list_.renaming()) SetFocus(list_.hwnd());
    rememberView();
    active_ = index;
    showTab();
}

// Moves the current tab to another place, the others closing up. Each tab takes its folder, history and view
// along: nothing is reloaded.
void App::moveTab(int to) {
    if (to == active_ || to < 0 || to >= int(tabs_.size())) return;
    Tab moved = std::move(tab());
    tabs_.erase(tabs_.begin() + active_);
    tabs_.insert(tabs_.begin() + to, std::move(moved));
    active_ = to;
    showTabs();
}

// The current tab is dragged along the bar, the cursor (x) over tab 'over': it moves there once the cursor is past
// that tab's middle, so a slip during a click moves nothing, and once it would land under the cursor: any sooner,
// tabs of different widths would swap back and forth.
void App::dragTab(int over, int x) {
    RECT from{}, to{};
    if (over == active_ || !TabCtrl_GetItemRect(tabBar_, active_, &from) || !TabCtrl_GetItemRect(tabBar_, over, &to))
        return;
    LONG width = from.right - from.left, middle = (to.left + to.right) / 2;
    if (over > active_ ? x >= std::max(to.right - width, middle) : x < std::min(to.left + width, middle)) moveTab(over);
}

// The folder of the tab under the cursor, for a drop. That tab shows as the current one while hovered (plain tabs
// take no highlight); the current tab's name keeps its bullet.
std::optional<std::wstring> App::tabDropTarget(const POINT* screenPt) {
    int hover = screenPt ? tabAt(tabBar_, *screenPt) : -1;
    if (hover != dropTab_) {
        TabCtrl_SetCurSel(tabBar_, hover >= 0 ? hover : active_);
        dropTab_ = hover;
    }
    if (hover < 0 || hover >= int(tabs_.size())) return std::nullopt;
    const Tab& t = tabs_[size_t(hover)];
    // Not into This PC, not into a folder that has not loaded here, and not back where the files come from.
    if (t.path.empty() || t.unavailable || (draggingFiles_ && samePath(t.path, dragSource_))) return std::nullopt;
    return t.path;
}

// Every tab is as wide as the others: 150 (96-DPI units), all narrower when that many don't fit, down to 80, and
// then the bar scrolls. The tabs sit side by side after 2 pixels of margin. Each name is cut to fit inside the tab's
// padding (6 a side), so no tab grows past the minimum width set here. The current tab's name starts with a bullet,
// as Windows' menus mark the chosen option: Windows draws that tab white and taller, which alone is easy to miss.
void App::showTabs() {
    dropTab_ = -1;   // the tabs are rebuilt, with the current one shown
    TabCtrl_DeleteAllItems(tabBar_);
    RECT bar{};
    GetClientRect(tabBar_, &bar);
    int share = (int(bar.right) - 2) / std::max(int(tabs_.size()), 1);
    int width = std::clamp(share, scale(80, dpi_), scale(150, dpi_));
    TabCtrl_SetMinTabWidth(tabBar_, width);
    HDC dc = GetDC(tabBar_);
    HGDIOBJ old = SelectObject(dc, font_.get());
    for (size_t i = 0; i < tabs_.size(); ++i) {
        std::wstring text = fitText(dc, (int(i) == active_ ? L"• " : L"") + displayName(tabs_[i].path), width - scale(12, dpi_));
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = text.data();
        TabCtrl_InsertItem(tabBar_, int(i), &item);
    }
    SelectObject(dc, old);
    ReleaseDC(tabBar_, dc);
    TabCtrl_SetCurSel(tabBar_, active_);
}

// The tab's folder as a shortcut in Bookmarks, shown in the tree at once (5.6).
void App::bookmarkTab(int index) {
    // On a worker: the tab's folder, and so writing the shortcut for it, may wait on a server that is off, as
    // saveTabs does. The reply shows the tree and the message.
    inBackground([wnd = wnd_, dir = tree_.bookmarks(), folder = tabs_[size_t(index)].path] {
        PostMessageW(wnd, WM_APP_BOOKMARKED, bookmarkFolder(dir, folder), 0);
    });
}

// ---------------------------------------------------------------- saved tabs (Bookmarks\Tabs)

std::vector<std::wstring> App::tabFolders() const {
    std::vector<std::wstring> folders;
    for (const Tab& t : tabs_) folders.push_back(t.path);
    return folders;
}

// Both work on a worker thread: a tab's folder may be on a server that is off, and so may Bookmarks.
void App::saveTabs() {
    inBackground([wnd = wnd_, dir = joinPath(tree_.bookmarks(), L"Tabs"), folders = tabFolders()] {
        PostMessageW(wnd, WM_APP_TABS_SAVED, saveFolderShortcuts(dir, folders), 0);
    });
}

void App::openSavedTabs() {
    inBackground([wnd = wnd_, dir = joinPath(tree_.bookmarks(), L"Tabs")] {
        post(wnd, WM_APP_TABS_READ, 0, std::make_unique<std::vector<std::wstring>>(savedFolders(dir)));
    });
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

// ---------------------------------------------------------------- file commands

void App::deleteSelection(bool permanently) {
    std::vector<int> rows = list_.selection();
    if (rows.empty() || list_.dir().empty()) return;
    // Afterwards, select the item that followed the deleted ones.
    int next = rowAfterDeleting(rows, list_.count());
    ViewState v = next >= 0 ? ViewState::selecting(list_.at(next).name) : ViewState{};
    v.top = list_.topRow();
    std::vector<Item> items = list_.selectedItems();
    std::wstring dir = list_.dir();
    // Windows recycles on a worker, so the window stays responsive for a big delete (4.7) and the hang watch stays
    // quiet. The selection is cleared now, keeping the focus on the item that will follow the deleted ones: a second
    // Delete meanwhile acts on nothing (4.6). The reply selects that item once the folder is read again.
    list_.select({}, v.focus);
    inBackground([wnd = wnd_, dir, items, permanently, v = std::move(v)]() mutable {
        bool ok = deleteItems(wnd, dir, items, permanently);
        post(wnd, WM_APP_DELETED, ok, std::make_unique<ViewState>(std::move(v)));
    });
}

void App::newFolder() {
    const std::wstring& dir = list_.dir();   // the folder shown (none for This PC or an unavailable folder)
    if (dir.empty()) return;
    std::wstring name = L"New folder";
    for (int i = 2; !CreateDirectoryW(joinPath(dir, name).c_str(), nullptr); ++i) {
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS || i > 999) {
            warn(errorText(error));   // 4.9: an impossible action beeps
            return;
        }
        name = L"New folder (" + std::to_wstring(i) + L")";
    }
    SetWindowTextW(filter_, L"");
    ViewState v = ViewState::selecting(name);
    v.rename = true;
    refresh(v);
}

// Ctrl+Shift+C (5.4): the selection's full paths as text, quoted and one per line as Explorer's Copy as path puts
// them; with nothing selected, the folder's own (none for This PC or a folder that isn't available).
void App::copyPaths() {
    std::vector<std::wstring> paths;
    for (const std::wstring& name : list_.selectedNames()) paths.push_back(joinPath(list_.dir(), name));
    if (paths.empty() && !list_.dir().empty()) paths.push_back(list_.dir());
    if (paths.empty()) return;
    std::wstring text;
    for (const std::wstring& p : paths) text += (text.empty() ? L"\"" : L"\r\n\"") + p + L"\"";
    if (copyText(wnd_, text)) setMessage(std::to_wstring(paths.size()) + L" path(s) copied");
    else warn(L"Can't put the paths on the clipboard");
}

// Ctrl+` (5.3): a terminal in the folder shown. Finding the program reads the PATH, so it is found and started on a
// worker, as a typed command is (4.1).
void App::openTerminal() {
    inBackground([wnd = wnd_, dir = terminalFolder()] {
        std::optional<Terminal> t = terminalProgram();
        PostMessageW(wnd, WM_APP_TERMINAL, t && startTerminal(wnd, *t, dir), 0);
    });
}

std::wstring App::terminalFolder() const {
    return list_.dir().empty() ? knownFolder(FOLDERID_Profile) : list_.dir();   // This PC, or a folder not available
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

void App::warn(const std::wstring& text) {
    setMessage(text);
    if (!selfTest_) MessageBeep(MB_ICONWARNING);   // self-tests trigger errors on purpose; keep them quiet
}

void App::setMessage(const std::wstring& text) {
    SendMessageW(status_, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(text.c_str()));
    loadMessageShown_ = false;   // a "Loading..." or "not available" there is gone (their callers set it again)
}

std::wstring App::message() const {
    std::wstring s(size_t(LOWORD(SendMessageW(status_, SB_GETTEXTLENGTHW, 1, 0))) + 1, L'\0');
    SendMessageW(status_, SB_GETTEXTW, 1, reinterpret_cast<LPARAM>(s.data()));
    return std::wstring(s.c_str());
}

void App::hint(bool& said, const wchar_t* text, bool afterLoad) {
    if (said) return;
    said = true;
    if (afterLoad) lateHint_ = text;   // onLoaded says it: the navigation clears messages first
    else setMessage(text);
}

// Open in new window (5.8): the folder in another Qoqdir, a title bar down and right of this window and the same size,
// maximized if this one is. The place goes in its STARTUPINFO, where create there makes the window instead of at the
// saved place (startAsk).
App::NewWindow App::newWindow(const std::wstring& dir) const {
    NewWindow w;
    w.exe = exePath();
    w.commandLine = L"\"" + w.exe + L"\" " + tabArgument(dir);
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(wnd_, MONITOR_DEFAULTTONEAREST), &mi);
    RECT from{};
    w.maximized = IsZoomed(wnd_) != 0;
    if (w.maximized) {   // the place it goes back to, which Windows counts from the work area's corner
        WINDOWPLACEMENT wp{};
        wp.length = sizeof wp;
        GetWindowPlacement(wnd_, &wp);
        from = wp.rcNormalPosition;
        OffsetRect(&from, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
    } else {
        GetWindowRect(wnd_, &from);
    }
    int titleBar = GetSystemMetricsForDpi(SM_CYCAPTION, dpi_) + GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi_) +
                   GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
    w.place = cascade(from, mi.rcWork, titleBar);
    return w;
}

void App::openInNewWindow(const std::wstring& dir) {
    // On a worker thread: the exe may live on a server, which can be slow to answer.
    inBackground([wnd = wnd_, w = newWindow(dir)]() mutable {
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USEPOSITION | STARTF_USESIZE | STARTF_USESHOWWINDOW;
        si.dwX = DWORD(w.place.left);
        si.dwY = DWORD(w.place.top);
        si.dwXSize = DWORD(w.place.right - w.place.left);
        si.dwYSize = DWORD(w.place.bottom - w.place.top);
        si.wShowWindow = WORD(w.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
        PROCESS_INFORMATION pi{};
        // The exe named apart: read from the command line, its path could be no longer than MAX_PATH.
        if (!CreateProcessW(w.exe.c_str(), w.commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                            &pi)) {
            PostMessageW(wnd, WM_APP_NEWWINDOW, GetLastError(), 0);
            return;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    });
}

// Missing, not a number, or far off any screen (a damaged file: a value stays within +/-100000, so sums like x + w
// don't overflow): the fallback. The window numbers in qoqdir.ini.
int App::iniNumber(const std::wstring& key, int fallback) const {
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"Window", key.c_str(), L"", text, 32, ini_.c_str());
    wchar_t* end = nullptr;
    long value = wcstol(text, &end, 10);
    return end != text && *end == 0 && value >= -100000 && value <= 100000 ? int(value) : fallback;
}

void App::loadSettings(const CommandLine& cmd, const StartAsk& ask) {
    treeWidth_ = iniNumber(L"TreeWidth", 220);
    list_.setShowHidden(iniNumber(L"ShowHidden", 0) != 0);
    int sortCol = iniNumber(L"SortColumn", 0);   // out of range falls back to Name, the default (5.11), as in Rust
    list_.setSort(SortKey(sortCol >= 0 && sortCol <= 3 ? sortCol : 0), iniNumber(L"SortAscending", 1) != 0);
    const int defaults[] = {320, 150, 170, 90};
    std::vector<int> widths;
    for (int i = 0; i < 4; ++i)
        widths.push_back(scale(iniNumber(L"Column" + std::to_wstring(i), defaults[i]), dpi_));
    list_.setColumnWidths(widths);

    // Tabs are not remembered: the command line's folders, or the home folder.
    for (const auto& folder : cmd.folders) tabs_.push_back(makeTab(folder));
    if (tabs_.empty()) tabs_.push_back(makeTab(knownFolder(FOLDERID_Profile)));
    active_ = 0;

    // The window is already at its place and size (create). Only show it now, maximized if asked (Open in new
    // window, 5.8) or saved. A self-test never steals the focus.
    bool quiet = !useSettings_ || ask.quiet;
    bool maximized = ask.place ? ask.maximized : iniNumber(L"Maximized", 0) != 0;
    ShowWindow(wnd_, quiet ? SW_SHOWNOACTIVATE : maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
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
        number(L"Column" + std::to_wstring(i), unscale(widths[i], dpi_));
}
