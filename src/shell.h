#pragma once
// Everything that talks to the Windows shell. Windows itself copies, deletes, renames and shows the menus,
// as in Explorer, so the app never re-implements (or gets wrong) any of that.

#include "folder.h"

#include <functional>
#include <optional>

// ---------------------------------------------------------------- shell objects

Pidl pidlFromPath(const std::wstring& path);   // "" is This PC; empty when Windows can't parse it

// ---------------------------------------------------------------- icons and types

HIMAGELIST systemImageList();                          // small icons, shared by the whole process
void setTheme(HWND control, const wchar_t* name);     // "Explorer": Explorer's look for a list or tree; "": plain
int typeIcon(const Item& it);                          // its type's icon; UI thread only, never touches the disk

// .exe, .lnk and the like have their own icon, read on a worker thread (it may need the network).
bool hasOwnIcon(const Item& it);
struct IconBatch {
    std::wstring dir;
    std::vector<std::pair<std::wstring, int>> icons;   // file name, icon
};
IconBatch readOwnIcons(const std::wstring& dir, const std::vector<std::wstring>& names);   // worker thread
int folderIcon();
// Each item's type name and, unless it has an icon of its own, its icon, from the shell's answer for the extension,
// asked once per extension. Worker thread: a file type's icon handler (a DLL some program installed) can hang, and
// asked here it hangs the load, not the window.
void fillTypes(std::vector<Item>& items);
const std::wstring& typeName(const Item& it);          // UI thread only (cached)
std::wstring knownFolder(REFKNOWNFOLDERID id);

// ---------------------------------------------------------------- menus

// One item, given by its full path, as a shell data object (what copy, drag and drop hand around).
Com<IDataObject> dataObject(HWND owner, const std::wstring& path);
// Below, taking the list's items (not paths) spares the shell a disk query per item.
// Context menus for items of one folder, for one item by its full path, or for a folder's background.
Com<IContextMenu> itemMenu(HWND owner, const std::wstring& dir, const std::vector<Item>& items);
Com<IContextMenu> itemMenu(HWND owner, const std::wstring& path);
Com<IContextMenu> backgroundMenu(HWND owner, const std::wstring& dir);
// An item of the app's own at the top of a shell menu (Paste on a folder's menu, for one).
struct OwnItem {
    const wchar_t* text;
    bool enabled;
    std::function<void()> act;
};
// 'intercept' sees the chosen verb first and returns true if it handled it. 'own' items come first.
void showMenu(HWND owner, IContextMenu* menu, POINT screenPt,
              const std::function<bool(const std::wstring& verb)>& intercept, const std::vector<OwnItem>& own = {});
// Forwards owner-drawn submenu messages (Send to, Open with) while a menu is open.
bool forwardMenuMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);
void invokeVerb(HWND owner, IContextMenu* menu, const char* verb);

// ---------------------------------------------------------------- file operations

// False when the clipboard could not be set: a path too long for the shell, or another program holds it.
bool copyToClipboard(HWND owner, const std::wstring& dir, const std::vector<Item>& items, bool cut);
void pasteInto(HWND owner, const std::wstring& dir, bool shortcuts = false);   // shortcuts: "Paste shortcut"
bool canPaste();   // the clipboard holds something Paste puts in a folder
// False when nothing was deleted: the user said No to Windows' question, or it failed.
bool deleteItems(HWND owner, const std::wstring& dir, const std::vector<Item>& items, bool permanently);
HRESULT renameItem(HWND owner, const std::wstring& path, const std::wstring& newName);
void openFile(HWND owner, const std::wstring& path);
// Explorer on the folder, or Windows Search scoped to it: what Qoqdir leaves to Explorer (thumbnails, searching
// inside subfolders).
void openInExplorer(HWND owner, const std::wstring& dir, bool search);
bool runCommand(HWND owner, const std::wstring& command, const std::wstring& workDir);
// What Ctrl+` starts in a folder (5.3): Windows Terminal ("wt -d ."), found as a typed name is, or else cmd. Finding
// it reads the PATH, so both run on a worker.
struct Terminal {
    std::wstring program, args;
};
std::optional<Terminal> terminalProgram();
bool startTerminal(HWND owner, const Terminal& terminal, const std::wstring& dir);
bool copyText(HWND owner, const std::wstring& text);   // the clipboard then holds just this text
// The full path a typed program name runs (PATH, then App Paths), nothing when it is on neither (so a file of
// that name in the folder shown is never run, 5.3); a path or a document name comes back unchanged. For runCommand
// and the self-test.
std::optional<std::wstring> commandProgram(const std::wstring& name);
void dragOut(HWND owner, const std::wstring& dir, const std::vector<Item>& items);

// ---------------------------------------------------------------- shortcuts

// A .lnk file's target, normalized like every way into a tab (a stored "D:\photos\" would give a blank caption and a
// stuck Up): a path, "" for This PC, or nothing (another file, a web link, a library); 'folder' if it was one. A
// target on a drive letter that is gone is looked for where the shortcut says it is relative to itself (a flash
// drive on another letter). No disk is asked: a server that is off costs nothing.
std::optional<std::wstring> shortcutTarget(const std::wstring& lnkPath, bool* folder);
// Saved tabs: shortcuts in 'dir' (Bookmarks\Tabs), numbered in tab order ("1 logs.lnk"); "" is This PC.
// Saving replaces the shortcuts there and leaves anything else. Worker thread: a folder may be on a server
// that is off.
bool saveFolderShortcuts(const std::wstring& dir, const std::vector<std::wstring>& folders);
std::vector<std::wstring> savedFolders(const std::wstring& dir);   // the folders they lead to, in order
// A bookmark: a shortcut to 'folder' in 'dir' (Bookmarks), named after the folder; one that is there already stays.
bool bookmarkFolder(const std::wstring& dir, const std::wstring& folder);

// ---------------------------------------------------------------- background work

// Runs `work` on a thread of its own, with COM set up for the shell calls it makes. The thread is never joined, so
// it can outlive the window and the App: `work` captures values and a window handle only, never a pointer to
// either, and reports with post(), which drops the message when the window is gone.
void inBackground(std::function<void()> work);
// Hands `object` to `window` by a posted message; the handler then owns it. Freed here if the window is gone.
template <class T>
void post(HWND window, UINT msg, WPARAM wp, std::unique_ptr<T> object) {
    if (PostMessageW(window, msg, wp, reinterpret_cast<LPARAM>(object.get()))) object.release();
}

// ---------------------------------------------------------------- folder watch

// Posts 'msg' to 'owner' when something in 'dir' changes, or 'dir' itself goes; stops when destroyed.
class FolderWatch {
public:
    FolderWatch() = default;
    FolderWatch(HWND owner, UINT msg, const std::wstring& dir);
    ~FolderWatch();
    FolderWatch(FolderWatch&& o) noexcept;
    FolderWatch& operator=(FolderWatch&& o) noexcept;
    FolderWatch(const FolderWatch&) = delete;
    FolderWatch& operator=(const FolderWatch&) = delete;
    bool watches(const std::wstring& dir) const { return dir_ && samePath(*dir_, dir); }   // "" is This PC
    // Its folder is in 'dir', which it watches too, for that folder going.
    bool watchesChildOf(const std::wstring& dir) const {
        return dir_ && !dir_->empty() && samePath(parentPath(*dir_), dir);
    }

private:
    ULONG ids_[2] = {};
    std::optional<std::wstring> dir_;   // none: it watches nothing, or Windows refused it (the next load tries again)
};
void acknowledgeChange(WPARAM wp, LPARAM lp);

// ---------------------------------------------------------------- drag and drop

// The local folder a tree node or a shortcut in the list stands for when files are dropped on it: its own, or the
// shortcut's target (read from the .lnk alone). None for This PC, a link to a file or a web page, a shortcut whose
// folder is gone, and anything on the network: asking Windows about a server that is off would freeze the drag.
std::optional<std::wstring> localFolder(std::wstring path, bool shortcut);

// Drops go to the folder 'target' returns for the cursor (none: refused); nullptr means the drag left or ended.
// 'dragged' has the paths of the files the drag carries, read from its data: wherever it started, the drop target
// itself refuses a folder they would go back to (4.8), and 'target' can use them to show that, as the tree does.
using DropTargetFn =
    std::function<std::optional<std::wstring>(const POINT* screenPt, const std::vector<std::wstring>& dragged)>;
Com<IDropTarget> newDropTarget(HWND owner, DropTargetFn target);

// ---------------------------------------------------------------- shell process

// Keeps the process alive while Windows finishes background copies started from the app.
class ShellProcessRef final : public IUnknown {
public:
    ShellProcessRef();
    ~ShellProcessRef();
    void waitForShell();   // call after the message loop ends
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

private:
    LONG refs_ = 1;
    HANDLE done_;
};
