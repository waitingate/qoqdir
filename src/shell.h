#pragma once
// Everything that talks to the Windows shell. Copying, deleting, renaming,
// context menus and icons are done by Windows itself, exactly as in Explorer,
// so the app never re-implements (and never gets wrong) any of that.

#include "folder.h"

#include <functional>
#include <optional>

HIMAGELIST systemImageList();                          // small icons, shared by the whole process
int iconFor(const std::wstring& path, const Item& it); // UI thread only; never touches the disk

// Files like .exe and .lnk carry their own icon, which needs a disk (maybe network)
// lookup. That runs on a worker thread; until it arrives the file shows its type's icon.
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

// The items as a shell data object (what copy, drag and drop hand around).
Com<IDataObject> dataObject(HWND owner, const std::vector<std::wstring>& paths);
// Context menu for items that share one parent folder, or for a folder's background.
Com<IContextMenu> itemMenu(HWND owner, const std::vector<std::wstring>& paths);
Com<IContextMenu> backgroundMenu(HWND owner, const std::wstring& dir);
// Shows the menu. 'intercept' sees the chosen verb first and returns true if it handled it.
void showMenu(HWND owner, IContextMenu* menu, POINT screenPt,
              const std::function<bool(const std::wstring& verb)>& intercept);
// Forwards owner-drawn submenu messages (Send to, Open with) while a menu is open.
bool forwardMenuMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);
bool invokeVerb(HWND owner, IContextMenu* menu, const char* verb);

void copyToClipboard(HWND owner, const std::vector<std::wstring>& paths, bool cut);
void pasteInto(HWND owner, const std::wstring& dir);
void deleteItems(HWND owner, const std::vector<std::wstring>& paths, bool permanently);
HRESULT renameItem(HWND owner, const std::wstring& path, const std::wstring& newName);
void openFile(HWND owner, const std::wstring& path);
bool runCommand(HWND owner, const std::wstring& command, const std::wstring& workDir);
void dragOut(HWND owner, const std::vector<std::wstring>& paths);
std::wstring shortcutTarget(const std::wstring& lnkPath);  // stored target path; no disk access

// Posts 'msg' to 'owner' when something inside 'dir' changes, or when 'dir' itself
// is deleted or renamed (seen from its parent). Stops watching when destroyed.
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

// A drop target that hands drops to whatever folder 'target' returns for the cursor
// position (screen coordinates; none = refuse). 'target' is called with nullptr when
// the drag leaves or ends, so it can remove any highlight it showed.
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
