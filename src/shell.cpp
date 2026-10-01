#include "shell.h"

#include <shellapi.h>

#include <algorithm>
#include <unordered_map>

namespace {

Pidl pidlFromPath(const std::wstring& path) {
    PIDLIST_ABSOLUTE p = nullptr;
    if (path.empty())
        SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &p);
    else
        SHParseDisplayName(path.c_str(), nullptr, &p, 0, nullptr);
    return Pidl(p);
}

using ChildPidl = std::unique_ptr<std::remove_pointer_t<PITEMID_CHILD>, CoTaskMemDeleter>;

// Asks the parent folder of 'paths' for an interface covering all of them
// (a context menu, a data object, a drop target...). All paths must share one parent.
// The parent is opened once and each item is looked up by name inside it: resolving
// every full path from the desktop down took ~9 ms per file (90 s for 10,000 files).
HRESULT uiObject(HWND owner, const std::vector<std::wstring>& paths, REFIID iid, void** out) {
    *out = nullptr;
    if (paths.empty()) return E_INVALIDARG;
    if (paths.size() == 1) {
        // One item: resolve its full path exactly. This also covers items without an
        // ordinary parent folder: This PC, and a network share's top (\server\share).
        Pidl pidl = pidlFromPath(paths[0]);
        Com<IShellFolder> parent;
        PCUITEMID_CHILD child = nullptr;
        if (!pidl || FAILED(SHBindToParent(pidl.get(), IID_IShellFolder, reinterpret_cast<void**>(parent.put()), &child)))
            return E_FAIL;
        return parent->GetUIObjectOf(owner, 1, &child, iid, nullptr, out);
    }
    Pidl parentPidl = pidlFromPath(parentPath(paths[0]));
    Com<IShellFolder> desktop, parent;
    if (!parentPidl || FAILED(SHGetDesktopFolder(desktop.put()))) return E_FAIL;
    HRESULT hr = parentPidl->mkid.cb == 0   // the parent is the desktop itself
                     ? desktop->QueryInterface(IID_IShellFolder, reinterpret_cast<void**>(parent.put()))
                     : desktop->BindToObject(parentPidl.get(), nullptr, IID_IShellFolder, reinterpret_cast<void**>(parent.put()));
    if (FAILED(hr)) return hr;
    std::vector<ChildPidl> owned;
    std::vector<PCUITEMID_CHILD> children;
    for (const auto& p : paths) {
        std::wstring name = leafName(p);   // "C:\" for a drive under This PC
        PIDLIST_RELATIVE child = nullptr;
        hr = parent->ParseDisplayName(owner, nullptr, name.data(), nullptr, &child, nullptr);
        if (FAILED(hr)) return hr;
        owned.emplace_back(static_cast<PITEMID_CHILD>(child));
        children.push_back(owned.back().get());
    }
    return parent->GetUIObjectOf(owner, UINT(children.size()), children.data(), iid, nullptr, out);
}


// Runs one IFileOperation: Windows shows progress, conflict and error dialogs, and records undo.
template <class AddWork>
HRESULT fileOperation(HWND owner, DWORD flags, AddWork addWork) {
    Com<IFileOperation> op;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_IFileOperation,
                                  reinterpret_cast<void**>(op.put()));
    if (FAILED(hr)) return hr;
    op->SetOwnerWindow(owner);
    op->SetOperationFlags(flags);
    hr = addWork(op.get());
    if (SUCCEEDED(hr)) hr = op->PerformOperations();
    return hr;
}

constexpr UINT kFirstCmd = 1, kLastCmd = 0x7FFF;          // the shell's own menu items
constexpr UINT kPasteCmd = 0x8000, kPasteShortcutCmd = 0x8001;   // ours
Com<IContextMenu2> g_menu2;   // set only while a context menu is on screen
Com<IContextMenu3> g_menu3;

}  // namespace

HIMAGELIST systemImageList() {
    SHFILEINFOW sfi{};
    return reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(L"x", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof sfi,
        SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
}

namespace {
struct TypeInfo {
    int icon = 0;
    std::wstring type;
};

// Icon and type name depend only on the extension, so ask the shell once per extension.
// Uses SHGFI_USEFILEATTRIBUTES: no disk access, never blocks.
const TypeInfo& typeInfo(const Item& it) {
    static std::unordered_map<std::wstring, TypeInfo> cache;
    std::wstring key = it.isDir() ? L"\\" : extensionOf(it.name);
    auto found = cache.find(key);
    if (found != cache.end()) return found->second;
    SHFILEINFOW sfi{};
    std::wstring probe = it.isDir() ? L"folder" : L"file" + key;
    SHGetFileInfoW(probe.c_str(), it.isDir() ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof sfi,
                   SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_TYPENAME);
    return cache.emplace(key, TypeInfo{sfi.iIcon, sfi.szTypeName}).first->second;
}
}  // namespace

namespace {
std::unordered_map<std::wstring, int> g_ownIcons;   // full path -> icon; UI thread only
}

bool hasOwnIcon(const Item& it) {
    static const wchar_t* const own[] = {L".exe", L".lnk", L".ico", L".url", L".cur",
                                         L".ani", L".msc", L".cpl", L".scr", L".appref-ms"};
    if (it.isDir()) return false;
    std::wstring ext = extensionOf(it.name);
    return std::any_of(std::begin(own), std::end(own), [&](const wchar_t* e) { return ext == e; });
}

IconBatch readOwnIcons(const std::wstring& dir, const std::vector<std::wstring>& names) {
    IconBatch batch{dir, {}};
    for (const auto& name : names) {
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(joinPath(dir, name).c_str(), 0, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
            batch.icons.emplace_back(name, sfi.iIcon);
    }
    return batch;
}

void rememberIcons(const IconBatch& batch) {
    for (const auto& [name, icon] : batch.icons) g_ownIcons[joinPath(batch.dir, name)] = icon;
}

int iconFor(const std::wstring& path, const Item& it) {
    if (hasOwnIcon(it)) {
        auto found = g_ownIcons.find(path);
        if (found != g_ownIcons.end()) return found->second;
    }
    return typeInfo(it).icon;
}

int folderIcon() {
    Item dir;
    dir.attrs = FILE_ATTRIBUTE_DIRECTORY;
    return typeInfo(dir).icon;
}

const std::wstring& typeName(const Item& it) {
    return it.type.empty() ? typeInfo(it).type : it.type;
}

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

Com<IDataObject> dataObject(HWND owner, const std::vector<std::wstring>& paths) {
    Com<IDataObject> obj;
    uiObject(owner, paths, IID_IDataObject, reinterpret_cast<void**>(obj.put()));
    return obj;
}

Com<IContextMenu> itemMenu(HWND owner, const std::vector<std::wstring>& paths) {
    Com<IContextMenu> cm;
    uiObject(owner, paths, IID_IContextMenu, reinterpret_cast<void**>(cm.put()));
    return cm;
}

Com<IContextMenu> backgroundMenu(HWND owner, const std::wstring& dir) {
    Com<IContextMenu> cm;
    Pidl pidl = pidlFromPath(dir);
    Com<IShellFolder> desktop, folder;
    if (!pidl || FAILED(SHGetDesktopFolder(desktop.put()))) return cm;
    if (FAILED(desktop->BindToObject(pidl.get(), nullptr, IID_IShellFolder, reinterpret_cast<void**>(folder.put()))))
        return cm;
    folder->CreateViewObject(owner, IID_IContextMenu, reinterpret_cast<void**>(cm.put()));
    return cm;
}

void showMenu(HWND owner, IContextMenu* cm, POINT pt, const std::function<bool(const std::wstring&)>& intercept,
              const std::function<void(bool shortcuts)>& paste) {
    if (!cm) return;
    Menu menu(CreatePopupMenu());
    if (!menu) return;
    if (paste) {   // greyed when no files were copied
        UINT state = MF_STRING | (IsClipboardFormatAvailable(CF_HDROP) ? 0 : MF_GRAYED);
        AppendMenuW(menu.get(), state, kPasteCmd, L"Paste\tCtrl+V");
        AppendMenuW(menu.get(), state, kPasteShortcutCmd, L"Paste shortcut");
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    }
    UINT flags = CMF_NORMAL | CMF_CANRENAME;
    if (GetKeyState(VK_SHIFT) < 0) flags |= CMF_EXTENDEDVERBS;
    if (FAILED(cm->QueryContextMenu(menu.get(), UINT(GetMenuItemCount(menu.get())), kFirstCmd, kLastCmd, flags))) return;

    g_menu2.reset();
    g_menu3.reset();
    cm->QueryInterface(IID_IContextMenu2, reinterpret_cast<void**>(g_menu2.put()));
    cm->QueryInterface(IID_IContextMenu3, reinterpret_cast<void**>(g_menu3.put()));
    UINT id = UINT(TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, owner, nullptr));
    g_menu2.reset();
    g_menu3.reset();
    if (id == kPasteCmd || id == kPasteShortcutCmd) return paste(id == kPasteShortcutCmd);
    if (id < kFirstCmd) return;

    wchar_t verb[80] = {};
    if (FAILED(cm->GetCommandString(id - kFirstCmd, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb), 80)))
        verb[0] = 0;
    if (intercept && intercept(verb)) return;

    CMINVOKECOMMANDINFOEX ici{};
    ici.cbSize = sizeof ici;
    ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
    if (GetKeyState(VK_CONTROL) < 0) ici.fMask |= CMIC_MASK_CONTROL_DOWN;
    if (GetKeyState(VK_SHIFT) < 0) ici.fMask |= CMIC_MASK_SHIFT_DOWN;
    ici.hwnd = owner;
    ici.lpVerb = MAKEINTRESOURCEA(id - kFirstCmd);
    ici.lpVerbW = MAKEINTRESOURCEW(id - kFirstCmd);
    ici.nShow = SW_SHOWNORMAL;
    ici.ptInvoke = pt;
    cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ici));
}

bool forwardMenuMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    if (g_menu3) {
        LRESULT r = 0;
        if (SUCCEEDED(g_menu3->HandleMenuMsg2(msg, wp, lp, &r))) {
            *result = r;
            return true;
        }
    } else if (g_menu2 && SUCCEEDED(g_menu2->HandleMenuMsg(msg, wp, lp))) {
        *result = 0;
        return true;
    }
    return false;
}

bool invokeVerb(HWND owner, IContextMenu* cm, const char* verb) {
    if (!cm) return false;
    Menu menu(CreatePopupMenu());
    if (!menu || FAILED(cm->QueryContextMenu(menu.get(), 0, kFirstCmd, kLastCmd, CMF_NORMAL))) return false;
    std::wstring verbW(verb, verb + strlen(verb));
    CMINVOKECOMMANDINFOEX ici{};
    ici.cbSize = sizeof ici;
    ici.fMask = CMIC_MASK_UNICODE;
    ici.hwnd = owner;
    ici.lpVerb = verb;
    ici.lpVerbW = verbW.c_str();
    ici.nShow = SW_SHOWNORMAL;
    return SUCCEEDED(cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ici)));
}

void copyToClipboard(HWND owner, const std::vector<std::wstring>& paths, bool cut) {
    Com<IDataObject> obj = dataObject(owner, paths);
    if (!obj) return;
    // Tells the paste target whether to move (cut) or copy.
    FORMATETC fmt{CLIPFORMAT(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)), nullptr, DVASPECT_CONTENT, -1,
                  TYMED_HGLOBAL};
    STGMEDIUM med{};
    med.tymed = TYMED_HGLOBAL;
    med.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (auto* effect = med.hGlobal ? static_cast<DWORD*>(GlobalLock(med.hGlobal)) : nullptr) {
        *effect = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY | DROPEFFECT_LINK;   // a copy also allows Paste shortcut
        GlobalUnlock(med.hGlobal);
        if (FAILED(obj->SetData(&fmt, &med, TRUE))) ReleaseStgMedium(&med);
    } else if (med.hGlobal) {
        GlobalFree(med.hGlobal);
    }
    OleSetClipboard(obj.get());
}

void pasteInto(HWND owner, const std::wstring& dir, bool shortcuts) {
    if (dir.empty()) return;
    // Same code path as Explorer's Paste (and Paste shortcut), with Windows' progress UI.
    invokeVerb(owner, backgroundMenu(owner, dir).get(), shortcuts ? "pastelink" : "paste");
}

void deleteItems(HWND owner, const std::vector<std::wstring>& paths, bool permanently) {
    Com<IDataObject> obj = dataObject(owner, paths);
    if (!obj) return;
    DWORD flags = permanently ? FOF_WANTNUKEWARNING : (FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION);
    fileOperation(owner, flags, [&](IFileOperation* op) { return op->DeleteItems(obj.get()); });
}

HRESULT renameItem(HWND owner, const std::wstring& path, const std::wstring& newName) {
    Com<IShellItem> item;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(item.put()));
    if (FAILED(hr)) return hr;
    return fileOperation(owner, FOF_ALLOWUNDO,
                         [&](IFileOperation* op) { return op->RenameItem(item.get(), newName.c_str(), nullptr); });
}

void openFile(HWND owner, const std::wstring& path) {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.hwnd = owner;
    sei.lpFile = path.c_str();
    std::wstring dir = parentPath(path);
    sei.lpDirectory = dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);   // Windows shows its own "open with" / error dialog
}

bool runCommand(HWND owner, const std::wstring& command, const std::wstring& workDir) {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.hwnd = owner;
    sei.lpDirectory = workDir.empty() ? nullptr : workDir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    sei.lpFile = command.c_str();
    if (ShellExecuteExW(&sei)) return true;
    // "notepad file.txt": program first, the rest as arguments.
    size_t space = command.find(L' ');
    if (space == std::wstring::npos) return false;
    std::wstring file = command.substr(0, space), args = command.substr(space + 1);
    sei.lpFile = file.c_str();
    sei.lpParameters = args.c_str();
    return ShellExecuteExW(&sei) != FALSE;
}

void dragOut(HWND owner, const std::vector<std::wstring>& paths) {
    Com<IDataObject> obj = dataObject(owner, paths);
    if (!obj) return;
    DWORD effect = 0;
    SHDoDragDrop(owner, obj.get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
}

std::wstring shortcutTarget(const std::wstring& lnkPath, bool* folder) {
    *folder = false;
    Com<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void**>(link.put()))))
        return L"";
    Com<IPersistFile> file = link.as<IPersistFile>();
    if (!file || FAILED(file->Load(lnkPath.c_str(), STGM_READ))) return L"";
    wchar_t target[MAX_PATH] = {};
    WIN32_FIND_DATAW stored{};   // the target as it was when the shortcut was made
    if (link->GetPath(target, MAX_PATH, &stored, 0) != S_OK) return L"";
    *folder = (stored.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return target;
}

namespace {
ULONG watch(HWND owner, UINT msg, const std::wstring& dir, LONG events) {
    Pidl pidl = pidlFromPath(dir);
    if (!pidl) return 0;
    SHChangeNotifyEntry entry{pidl.get(), FALSE};
    return SHChangeNotifyRegister(owner, SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery, events,
                                  msg, 1, &entry);
}
}  // namespace

FolderWatch::FolderWatch(HWND owner, UINT msg, const std::wstring& dir) {
    ids_[0] = watch(owner, msg, dir,
                    SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR | SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER |
                        SHCNE_UPDATEITEM | SHCNE_UPDATEDIR | SHCNE_ATTRIBUTES | SHCNE_DRIVEADD | SHCNE_DRIVEREMOVED |
                        SHCNE_MEDIAINSERTED | SHCNE_MEDIAREMOVED);
    if (!dir.empty())
        ids_[1] = watch(owner, msg, parentPath(dir),
                        SHCNE_RMDIR | SHCNE_RENAMEFOLDER | SHCNE_DRIVEREMOVED | SHCNE_MEDIAREMOVED);
}

FolderWatch::~FolderWatch() {
    for (ULONG id : ids_)
        if (id) SHChangeNotifyDeregister(id);
}

FolderWatch::FolderWatch(FolderWatch&& o) noexcept {
    std::swap(ids_, o.ids_);
}

FolderWatch& FolderWatch::operator=(FolderWatch&& o) noexcept {
    std::swap(ids_, o.ids_);
    return *this;
}

void acknowledgeChange(WPARAM wp, LPARAM lp) {
    PIDLIST_ABSOLUTE* pidls = nullptr;
    LONG event = 0;
    HANDLE lock = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wp), DWORD(lp), &pidls, &event);
    if (lock) SHChangeNotification_Unlock(lock);
}

namespace {

class DropTarget final : public IDropTarget {
public:
    DropTarget(HWND owner, DropTargetFn target) : owner_(owner), target_(std::move(target)) {
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_IDropTargetHelper,
                         reinterpret_cast<void**>(helper_.put()));
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return ULONG(n);
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
        data_.reset();
        if (data) {
            data->AddRef();
            *data_.put() = data;
        }
        aim(keys, pt, effect);
        POINT p{pt.x, pt.y};
        if (helper_) helper_->DragEnter(owner_, data, &p, *effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override {
        // Over a new folder, aim() has already asked it; otherwise the same folder decides again.
        if (!aim(keys, pt, effect) && (!folder_ || FAILED(folder_->DragOver(keys, pt, effect))))
            *effect = DROPEFFECT_NONE;
        POINT p{pt.x, pt.y};
        if (helper_) helper_->DragOver(&p, *effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (helper_) helper_->DragLeave();
        finish();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
        POINT p{pt.x, pt.y};
        if (helper_) helper_->Drop(data, &p, *effect);
        Com<IDropTarget> folder = folder_;
        target_(nullptr);   // the drag is over (e.g. remove highlights)
        if (!folder || FAILED(folder->Drop(data, keys, pt, effect))) *effect = DROPEFFECT_NONE;
        folder_.reset();
        dir_.reset();
        data_.reset();
        return S_OK;
    }

private:
    // Points the drop at the folder under the cursor. Returns true if that changed
    // (then the new folder has already been told about the drag).
    bool aim(DWORD keys, POINTL pt, DWORD* effect) {
        POINT p{pt.x, pt.y};
        std::optional<std::wstring> dir = target_(&p);
        if (dir == dir_) return false;
        if (folder_) folder_->DragLeave();
        folder_.reset();
        dir_ = dir;
        if (dir) uiObject(owner_, {*dir}, IID_IDropTarget, reinterpret_cast<void**>(folder_.put()));
        if (folder_ && FAILED(folder_->DragEnter(data_.get(), keys, pt, effect))) folder_.reset();
        if (!folder_) *effect = DROPEFFECT_NONE;
        return true;
    }
    void finish() {
        target_(nullptr);
        if (folder_) folder_->DragLeave();
        folder_.reset();
        dir_.reset();
        data_.reset();
    }

    LONG refs_ = 1;
    HWND owner_;
    DropTargetFn target_;
    Com<IDataObject> data_;
    std::optional<std::wstring> dir_;   // the folder the drop currently goes to
    Com<IDropTarget> folder_;           // and its shell drop target
    Com<IDropTargetHelper> helper_;
};

}  // namespace

IDropTarget* newDropTarget(HWND owner, DropTargetFn target) {
    return new DropTarget(owner, std::move(target));
}

ShellProcessRef::ShellProcessRef() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    SHSetInstanceExplorer(this);
}

ShellProcessRef::~ShellProcessRef() {
    if (done_) CloseHandle(done_);
}

void ShellProcessRef::waitForShell() {
    SHSetInstanceExplorer(nullptr);   // first, so nothing new can take a reference
    Release();                        // drop our own; background copies hold the others
    if (!done_) return;
    while (MsgWaitForMultipleObjects(1, &done_, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}

HRESULT ShellProcessRef::QueryInterface(REFIID iid, void** out) {
    if (iid == IID_IUnknown) {
        *out = this;
        AddRef();
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG ShellProcessRef::AddRef() { return ULONG(InterlockedIncrement(&refs_)); }

ULONG ShellProcessRef::Release() {
    LONG n = InterlockedDecrement(&refs_);
    if (n == 0 && done_) SetEvent(done_);
    return ULONG(n);
}
