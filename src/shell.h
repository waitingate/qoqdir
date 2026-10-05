#pragma once
// Everything that talks to the Windows shell. Windows itself copies, deletes, renames and shows the menus,
// as in Explorer, so the app never re-implements (or gets wrong) any of that.

#include "folder.h"

#include <functional>
#include <optional>

HIMAGELIST systemImageList();                          // small icons, shared by the whole process
void useExplorerTheme(HWND control);                   // Explorer's look for a list or tree
int iconFor(const std::wstring& path, const Item& it); // UI thread only; never touches the disk

// .exe, .lnk and the like have their own icon, read on a worker thread (it may need the network).
bool hasOwnIcon(const Item& it);
struct IconBatch {
    std::wstring dir;
    std::vector<std::pair<std::wstring, int>> icons;   // file name, icon
};
IconBatch readOwnIcons(const std::wstring& dir, const std::vector<std::wstring>& names);   // worker thread
void rememberIcons(const IconBatch& batch);                                                 // UI thread
int folderIcon();
const std::wstring& typeName(const Item& it);          // UI thread only (cached)
std::wstring knownFolder(REFKNOWNFOLDERID id);

// One item, given by its full path, as a shell data object (what copy, drag and drop hand around).
Com<IDataObject> dataObject(HWND owner, const std::wstring& path);
// Below, taking the list's items (not paths) spares the shell a disk query per item.
// Context menus for items of one folder, for one item by its full path, or for a folder's background.
Com<IContextMenu> itemMenu(HWND owner, const std::wstring& dir, const std::vector<Item>& items);
Com<IContextMenu> itemMenu(HWND owner, const std::wstring& path);
Com<IContextMenu> backgroundMenu(HWND owner, const std::wstring& dir);
// 'intercept' sees the chosen verb first and returns true if it handled it.
// With 'paste', Paste and Paste shortcut come first (a folder's own menu has neither).
void showMenu(HWND owner, IContextMenu* menu, POINT screenPt,
              const std::function<bool(const std::wstring& verb)>& intercept,
              const std::function<void(bool shortcuts)>& paste = nullptr);
// Forwards owner-drawn submenu messages (Send to, Open with) while a menu is open.
bool forwardMenuMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);
bool invokeVerb(HWND owner, IContextMenu* menu, const char* verb);

// False when the clipboard could not be set: a path too long for the shell, or another program holds it.
bool copyToClipboard(HWND owner, const std::wstring& dir, const std::vector<Item>& items, bool cut);
void pasteInto(HWND owner, const std::wstring& dir, bool shortcuts = false);   // shortcuts: "Paste shortcut"
// False when nothing was deleted: the user said No to Windows' question, or it failed.
bool deleteItems(HWND owner, const std::wstring& dir, const std::vector<Item>& items, bool permanently);
HRESULT renameItem(HWND owner, const std::wstring& path, const std::wstring& newName);
void openFile(HWND owner, const std::wstring& path);
bool runCommand(HWND owner, const std::wstring& command, const std::wstring& workDir);
void dragOut(HWND owner, const std::wstring& dir, const std::vector<Item>& items);
// A shortcut's stored target: a path, "" for This PC, or nothing (a web link, a library); 'folder' if it
// was one. Reads only the .lnk file, never the target, so a server that is off costs nothing.
std::optional<std::wstring> shortcutTarget(const std::wstring& lnkPath, bool* folder);
// Saved tabs: shortcuts in 'dir' (Bookmarks\Tabs), numbered in tab order ("1 logs.lnk"); "" is This PC.
// Saving replaces the shortcuts there and leaves anything else. Worker thread: a folder may be on a server
// that is off.
bool saveFolderShortcuts(const std::wstring& dir, const std::vector<std::wstring>& folders);
std::vector<std::wstring> savedFolders(const std::wstring& dir);   // the folders they lead to, in order

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

private:
    ULONG ids_[2] = {};
};
void acknowledgeChange(WPARAM wp, LPARAM lp);

// Drops go to the folder 'target' returns for the cursor (none: refused); nullptr means the drag left or ended.
using DropTargetFn = std::function<std::optional<std::wstring>(const POINT* screenPt)>;
IDropTarget* newDropTarget(HWND owner, DropTargetFn target);

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
