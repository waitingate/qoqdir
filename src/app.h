#pragma once
// The main window: toolbar, tabs, tree, file list and status bar.
//
// Threading rule: all state below is touched only by the UI thread. Folders are
// read on short-lived worker threads that own nothing shared; each result is
// posted back tagged with a generation number, and anything but the newest
// request is dropped. Navigation only changes state once a folder has loaded,
// so a failed or slow load can never leave a tab half-updated.

#include "filelist.h"

#include <optional>
#include "shell.h"
#include "tree.h"

enum class Nav { New, Back, Forward, Refresh, Switch };

struct ViewState {                  // what to select and scroll to after a folder loads
    std::vector<std::wstring> select;
    std::wstring focus;
    int top = -1;
    bool rename = false;            // start renaming 'focus'

    static ViewState selecting(const std::wstring& name) { return {{name}, name}; }
};

// Where a load was started from, when that changes what happens if it is not a folder.
struct Origin {
    bool typed = false;      // typed in the address bar: a file is opened instead
    std::wstring shortcut;   // opened from this shortcut: if it leads to a file, Windows opens the shortcut

    static Origin addressBar() { return {true, {}}; }
    static Origin fromShortcut(std::wstring lnk) { return {false, std::move(lnk)}; }
};

struct Tab {
    int id = 0;                     // stable identity; indexes shift when tabs close
    std::wstring path;              // "" = This PC
    std::vector<std::wstring> back, forward;
    ViewState view;                 // remembered while the tab is in the background
    bool unavailable = true;        // not loaded here yet, or failed: no drops onto it (asking Windows could freeze)
};

class App {
public:
    App();
    ~App();
    // Opens the command line's folders as tabs (or the home folder).
    bool create(HINSTANCE instance, const CommandLine& cmd, bool useSettings = true);
    bool handleKey(const MSG& msg);   // called for every message before dispatch

    // Self-test: drives the app from inside (no global keyboard input) and logs results.
    static std::wstring prepareSelfTest();   // makes a fresh test folder, returns its path
    void startSelfTest(const std::wstring& dir, const std::wstring& logPath);
    int exitCode() const { return exitCode_; }

private:
    struct Key {
        UINT message;   // WM_KEYDOWN, WM_SYSKEYDOWN or WM_CHAR
        WPARAM code;    // virtual key, or the character
        HWND focus;
        bool ctrl, shift, alt;
    };
    bool onKey(const Key& key);

    struct SelfTest;
    std::unique_ptr<SelfTest> selfTest_;
    void selfTestTick();
    void selfTestLog(const std::wstring& line);   // appends to the log file at once
    void selfTestKey(WPARAM vk, bool ctrl = false, bool shift = false, bool alt = false);
    void selfTestType(const std::wstring& text);
    static constexpr UINT_PTR kRefreshTimer = 1, kStatusTimer = 2, kLoadingTimer = 3, kSelfTestTimer = 50;
    int exitCode_ = 0;
    bool useSettings_ = true;

    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT onMessage(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT onNotify(NMHDR* nm);
    void onContextMenu(HWND from, POINT pt);
    void onTabMenu();
    void onTimer(UINT_PTR id);
    void queueRefresh();   // one coalesced refresh of the current tab, once no rename or load is in progress
    void onMiddleClick(HWND from, POINT screenPt);

    // Navigation
    void open(const std::wstring& path, Nav nav, ViewState view = {}, Origin origin = {});
    void onLoaded(Listing&& listing, UINT generation);
    struct Pending;
    void onLoadFailed(const Listing& listing, const Pending& p);
    void showLocation(Nav nav);
    void goBack();
    void goForward();
    void goUp();
    void refresh(ViewState view = {});
    void reload();   // like refresh, but also restarts a load in progress (e.g. after Ctrl+H)
    void goToTyped();
    void activate(bool inNewTab);   // Enter / double-click on the selection
    void openShortcut(const std::wstring& path, bool inNewTab);
    void openNode(const std::wstring& path, bool inNewTab);   // a tree node: folder or bookmark
    ViewState currentView() const;

    // Tabs
    Tab& tab() { return tabs_[size_t(active_)]; }
    Tab makeTab(const std::wstring& path);
    void newTab(const std::wstring& path, bool switchTo);
    void closeTab(int index);
    void closeOtherTabs(int index);
    void switchTab(int index);
    void showTabs();
    std::optional<std::wstring> tabDropTarget(const POINT* screenPt);

    // File commands
    void deleteSelection(bool permanently);
    void newFolder();
    void focusList();

    // Window
    void applyFont();
    void layout();
    void showStatus();
    void setMessage(const std::wstring& text);
    void warn();                    // error beep
    void loadSettings(const CommandLine& cmd);
    void saveSettings();

    HWND wnd_ = nullptr, back_ = nullptr, forward_ = nullptr, up_ = nullptr, address_ = nullptr, filter_ = nullptr,
         tabBar_ = nullptr, plus_ = nullptr, status_ = nullptr, lastFocus_ = nullptr;
    FileList list_;
    FolderTree tree_;
    Com<IDropTarget> tabDrop_;      // files dropped on a tab go to its folder
    int dropTab_ = -1;              // tab highlighted during a drag
    Font font_;
    UINT dpi_ = 96;
    int rowHeight_ = 0, tabHeight_ = 0;
    int treeWidth_ = 220;           // in 96-DPI units
    int splitterX_ = 0;
    bool dragging_ = false;         // moving the tree/list splitter
    bool draggingFiles_ = false;    // a drag started from our own list is in progress
    bool refreshQueued_ = false;
    struct QueuedView {             // kept by refresh() while a load is pending: what a delete, rename or new
        std::wstring dir;           // folder wanted selected. The queued refresh applies it if 'dir' is still shown.
        ViewState view;
    } queuedView_;
    bool loadMessageShown_ = false; // "Loading..." or "not available": cleared when a folder loads
    bool showHidden_ = false;

    std::vector<Tab> tabs_;
    int active_ = 0;
    int nextTabId_ = 1;

    struct Pending {
        UINT generation = 0;
        std::wstring path;
        Nav nav = Nav::New;
        ViewState view;
        int tabId = 0;
        Origin origin;
    } pending_;
    UINT generation_ = 0;

    struct Rename {                 // done after the edit box has closed
        std::wstring path, name;
    } rename_;

    FolderWatch watch_;
    std::wstring ini_;
};
