#pragma once
// The main window: toolbar, tabs, tree, file list and status bar.
// All state is UI-thread only. Folders load on worker threads; only the newest result is used, and a tab
// changes only once its folder has loaded, so a failed or slow load never leaves it half-updated.

#include "filelist.h"
#include "shell.h"
#include "tree.h"

#include <optional>

enum class Nav { New, Back, Forward, Refresh, Switch };

struct ViewState {                  // what to select and scroll to after a folder loads
    std::vector<std::wstring> select;
    std::wstring focus;
    int top = -1;
    int focusRow = -1;              // where 'focus' was: when it is gone, the item in its place takes the focus
    bool rename = false;            // start renaming 'focus'

    static ViewState selecting(const std::wstring& name) { return {{name}, name}; }
    bool asks() const { return !select.empty() || !focus.empty(); }   // for an item, as a rename or new folder does
};

// Where a load was started from, when that changes what happens if it is not a folder.
struct Origin {
    bool typed = false;      // typed in the address bar: a file is opened instead
    std::wstring shortcut;   // opened from this shortcut: if it leads to a file, Windows opens the shortcut
    bool restore = false;    // brings back the tab's own view, as a tab switch would (restoreTab)

    static Origin addressBar() { return {true, {}}; }
    static Origin fromShortcut(std::wstring lnk) { return {false, std::move(lnk)}; }
    static Origin tabRestore() { return {false, {}, true}; }
};

struct Tab {
    int id = 0;                     // stable identity; indexes shift when tabs close
    std::wstring path;              // "" = This PC
    std::vector<std::wstring> back, forward;
    ViewState view;                 // remembered while the tab is in the background
    bool unavailable = true;        // not loaded here yet, or failed: no drops onto it (asking Windows could freeze)
};

void showKeys(HWND owner);   // F1's help box: the main keys (main.cpp; qoqdir --help shows the command line)

inline std::wstring windowText(HWND h) {
    std::wstring s(size_t(GetWindowTextLengthW(h)) + 1, L'\0');
    s.resize(size_t(GetWindowTextW(h, s.data(), int(s.size()))));
    return s;
}

class App {
public:
    // Opens the command line's folders as tabs (or the home folder).
    bool create(HINSTANCE instance, const CommandLine& cmd, bool useSettings = true);
    bool handleKey(const MSG& msg);   // called for every message before dispatch

    // Self-test (in qoqdir-test.exe only): drives the app from inside (no global keyboard input) and logs results.
    static std::wstring prepareSelfTest();   // makes a fresh test folder, returns its path
    void startSelfTest(const std::wstring& dir, const std::wstring& logPath);
    int exitCode() const { return exitCode_; }

private:
    // Setup
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    void applyFont();
    void applyColors(bool highContrast);
    void layout();

    // Messages
    LRESULT onMessage(UINT msg, WPARAM wp, LPARAM lp);
    void queueRefresh();   // one coalesced refresh of the current tab, once no rename or load is in progress
    void onFolderChanged();   // the folder watch reported a change
    void onTimer(UINT_PTR id);
    void onMiddleClick(HWND from, POINT screenPt);
    LRESULT onNotify(NMHDR* nm);
    void onContextMenu(HWND from, POINT pt);
    void onTabMenu();

    // Keyboard
    struct Key {
        UINT message;   // WM_KEYDOWN, WM_SYSKEYDOWN or WM_CHAR
        WPARAM code;    // virtual key, or the character
        HWND focus;
        bool ctrl, shift, alt;
    };
    bool onKey(const Key& key);
    void focusList();

    // Navigation
    void open(const std::wstring& path, Nav nav, ViewState view = {}, Origin origin = {});
    void onLoaded(Listing&& listing, UINT generation);
    struct Pending;
    void onLoadFailed(const Listing& listing, const Pending& p);
    void restoreTab();   // a failed move left the list a tab switch emptied: read the tab's folder again
    void showLocation(Nav nav);
    void goBack();
    void goForward();
    bool goUp();   // false: at This PC, nowhere to go
    bool refresh(ViewState view = {});   // false: a load was in progress, so none was started
    // F5 and the Refresh button: the list (unless it is loading already) and the tree (5.5); the status bar says so.
    void refreshAll() { bool list = refresh(); tree_.refresh(); if (list) setMessage(L"Refreshed"); }
    bool loading() const { return pending_.generation != 0; }   // a folder read is in flight for this window
    void showTab() { open(tab().path, Nav::Switch, tab().view); }   // the current tab's folder into the list
    void showUnavailable(const std::wstring& path, DWORD error, Nav nav);   // the tab's folder can't be read (4.3)
    std::vector<std::wstring> tabFolders() const;
    void goToTyped();
    void activate(bool inNewTab);   // Enter / double-click on the selection
    void openShortcut(const std::wstring& path, bool inNewTab);
    void renameNode(const std::wstring& path);                // a tree node's Rename, done in the file list
    void openNode(const std::wstring& path, bool inNewTab);   // a tree node: folder or bookmark
    ViewState currentView() const;
    void rememberView();   // the tab leaves the screen: keep its selection and scroll for when it comes back

    // Tabs
    Tab& tab() { return tabs_[size_t(active_)]; }
    Tab makeTab(const std::wstring& path);
    void newTab(std::wstring path, bool switchTo);   // by value: adding the tab can move the one 'path' is in
    void closeTab(int index);
    void closeOtherTabs(int index);
    void switchTab(int index);
    void moveTab(int to);   // the current tab: Ctrl+Shift+PgUp / PgDn, or dragged along the bar
    void dragTab(int over, int x);
    std::optional<std::wstring> tabDropTarget(const POINT* screenPt);
    void showTabs();
    void bookmarkTab(int index);   // the tab's folder as a shortcut in Bookmarks (the tab's menu)

    // Saved tabs (Bookmarks\Tabs)
    void saveTabs();       // Ctrl+S: a shortcut per tab in Bookmarks\Tabs
    void openSavedTabs();  // Ctrl+O: their folders as tabs after the open ones
    void addSavedTabs(const std::vector<std::wstring>& folders);

    // File commands
    void deleteSelection(bool permanently);
    void newFolder();

    // Status and settings
    void showStatus();
    void setMessage(const std::wstring& text);
    // A status message once per run (5.3); with 'afterLoad', once the folder being loaded has landed.
    void hint(bool& said, const wchar_t* text, bool afterLoad = false);
    void warn(const std::wstring& text);   // an error: in the status bar, with a beep
    void loadSettings(const CommandLine& cmd);
    void saveSettings();

    // Self-test (selftest.cpp)
    struct SelfTest;
    std::shared_ptr<SelfTest> selfTest_;   // shared_ptr: it can be destroyed where SelfTest isn't defined
    void selfTestTick();
    void selfTestLog(const std::wstring& line);   // appends to the log file at once
    void selfTestKey(WPARAM vk, bool ctrl = false, bool shift = false, bool alt = false);
    void selfTestType(const std::wstring& text);

    static constexpr UINT_PTR kRefreshTimer = 1, kStatusTimer = 2, kLoadingTimer = 3, kSelfTestTimer = 50;
    int exitCode_ = 1;   // a run that never finishes (closed by hand, no window) has not passed
    bool useSettings_ = true;

    HWND wnd_ = nullptr, back_ = nullptr, forward_ = nullptr, up_ = nullptr, refresh_ = nullptr, address_ = nullptr,
         filter_ = nullptr, tabBar_ = nullptr, plus_ = nullptr, status_ = nullptr, tips_ = nullptr, lastFocus_ = nullptr;
    FileList list_;
    FolderTree tree_;
    Com<IDropTarget> tabDrop_;      // files dropped on a tab go to its folder
    Com<IDropTarget> treeDrop_;     // and on a folder in the tree, to that folder
    int dropTab_ = -1;              // tab highlighted during a drag
    int wheel_ = 0;                 // mouse wheel turned over the tab bar, not yet a whole step (WHEEL_DELTA)
    Font font_, arrowFont_;
    UINT dpi_ = 96;
    int rowHeight_ = 0, tabHeight_ = 0;
    int treeWidth_ = 220;           // in 96-DPI units
    int splitterX_ = 0;
    static constexpr int kSplitterWidth = 5;   // in 96-DPI units
    bool overSplitter(int x) const { return x >= splitterX_ && x < splitterX_ + scale(kSplitterWidth, dpi_); }
    bool hintedFilter_ = false, hintedUp_ = false;   // the two things Explorer users don't expect, said once (5.3)
    bool dragging_ = false;         // moving the tree/list splitter
    bool draggingFiles_ = false;    // a drag started from our own list is in progress
    bool refreshQueued_ = false;
    bool refreshForWatch_ = false;   // ...and only the folder watch asked for it
    bool listEmptied_ = false;       // a tab switch emptied the list, and no folder has filled it since
    UINT restoreRead_ = 0;           // the read restoreTab() started, until it comes back
    std::wstring restorePath_;       // ...and the folder it reads
    std::wstring restoreSaid_;       // what the failed move said, said again once the tab's folder is back
    struct QueuedView {             // kept by refresh() while a load is pending: what a delete, rename or new
        std::wstring dir;           // folder wanted selected. The queued refresh applies it if 'dir' is still shown.
        ViewState view;
    } queuedView_;
    bool loadMessageShown_ = false; // "Loading..." or "not available": cleared when a folder loads
    const wchar_t* lateHint_ = nullptr;   // a hint for when the folder being loaded lands (a navigation clears messages)

    std::vector<Tab> tabs_;
    int active_ = 0;
    int nextTabId_ = 1;

    struct Pending {
        UINT generation = 0;
        Nav nav = Nav::New;
        ViewState view;
        int tabId = 0;
        Origin origin;
        std::wstring filter;        // the filter box before this load cleared it: a failed one puts it back
    } pending_;
    UINT generation_ = 0;
    int loadsDone_ = 0;     // folder reads that reported back (self-test)
    int loadsWasted_ = 0;   // ...of which a newer load had replaced, yet they read the whole folder (self-test)
    UINT uncountedLoad_ = 0;   // a read the self-test leaves stuck on a network timeout: in neither count

    struct Rename {                 // done after the edit box has closed
        std::wstring path, name;
    } rename_;

    FolderWatch watch_;
    FolderWatch bookmarksWatch_;   // the Bookmarks node follows its folder whatever the list shows (5.5)
    std::wstring ini_;
};
