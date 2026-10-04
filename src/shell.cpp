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

// An interface (menu, data object, drop target...) on one item by its full path; covers This PC and \\server\share.
HRESULT uiObject(HWND owner, const std::wstring& path, REFIID iid, void** out) {
    *out = nullptr;
    Pidl pidl = pidlFromPath(path);
    Com<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    if (!pidl || FAILED(SHBindToParent(pidl.get(), IID_IShellFolder, reinterpret_cast<void**>(parent.put()), &child)))
        return E_FAIL;
    return parent->GetUIObjectOf(owner, 1, &child, iid, nullptr, out);
}

// The listing's find data, handed to the shell (STR_FILE_SYS_BIND_DATA) so parsing a name needs no disk query.
class FindData final : public IFileSystemBindData {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == IID_IFileSystemBindData) {
            *out = static_cast<IFileSystemBindData*>(this);
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
    HRESULT STDMETHODCALLTYPE SetFindData(const WIN32_FIND_DATAW* fd) override {
        fd_ = *fd;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFindData(WIN32_FIND_DATAW* fd) override {
        *fd = fd_;
        return S_OK;
    }

private:
    LONG refs_ = 1;
    WIN32_FIND_DATAW fd_{};
};

// The same for listed items of one folder: opened once, each item parsed from its own find data. Full
// paths took ~9 ms per file (90 s for 10,000), plain names a disk query each (spec 4.7).
HRESULT uiObject(HWND owner, const std::wstring& dir, const std::vector<Item>& items, REFIID iid, void** out) {
    *out = nullptr;
    if (items.empty()) return E_INVALIDARG;
    if (items.size() == 1) return uiObject(owner, joinPath(dir, items[0].name), iid, out);
    Pidl dirPidl = pidlFromPath(dir);
    Com<IShellFolder> desktop, parent;
    if (!dirPidl || FAILED(SHGetDesktopFolder(desktop.put()))) return E_FAIL;
    HRESULT hr = dirPidl->mkid.cb == 0   // the folder is the desktop itself
                     ? desktop->QueryInterface(IID_IShellFolder, reinterpret_cast<void**>(parent.put()))
                     : desktop->BindToObject(dirPidl.get(), nullptr, IID_IShellFolder, reinterpret_cast<void**>(parent.put()));
    if (FAILED(hr)) return hr;
    // STGM_CREATE: take the find data as given. (RegisterObjectParam never writes its non-const key.)
    Com<IFileSystemBindData> known;
    *known.put() = new FindData;
    Com<IBindCtx> bind;
    BIND_OPTS opts{};
    opts.cbStruct = sizeof opts;
    opts.grfMode = STGM_CREATE;
    if (FAILED(CreateBindCtx(0, bind.put())) || FAILED(bind->SetBindOptions(&opts)) ||
        FAILED(bind->RegisterObjectParam(const_cast<LPOLESTR>(STR_FILE_SYS_BIND_DATA), known.get())))
        return E_FAIL;
    std::vector<ChildPidl> owned;
    std::vector<PCUITEMID_CHILD> children;
    for (const Item& it : items) {
        WIN32_FIND_DATAW fd{};
        fd.dwFileAttributes = it.attrs;
        fd.ftLastWriteTime = it.modified;
        fd.nFileSizeHigh = DWORD(it.size >> 32);
        fd.nFileSizeLow = DWORD(it.size);
        it.name.copy(fd.cFileName, MAX_PATH - 1);   // "C:\" for a drive under This PC
        known->SetFindData(&fd);
        PIDLIST_RELATIVE child = nullptr;
        hr = parent->ParseDisplayName(owner, bind.get(), fd.cFileName, nullptr, &child, nullptr);
        if (FAILED(hr)) return hr;
        owned.emplace_back(static_cast<PITEMID_CHILD>(child));
        children.push_back(owned.back().get());
    }
    return parent->GetUIObjectOf(owner, UINT(children.size()), children.data(), iid, nullptr, out);
}

// Interface I on what uiObject finds (empty when it finds nothing).
template <class I, class... Where>
Com<I> shellObject(HWND owner, const Where&... where) {
    Com<I> obj;
    uiObject(owner, where..., __uuidof(I), reinterpret_cast<void**>(obj.put()));
    return obj;
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
    BOOL aborted = FALSE;   // the user answered No or Cancel: nothing (or not everything) was done
    if (SUCCEEDED(hr) && SUCCEEDED(op->GetAnyOperationsAborted(&aborted)) && aborted) hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
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

// Icon and type name depend only on the extension: one cached shell call each, with no disk access.
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

std::unordered_map<std::wstring, int> g_ownIcons;   // full path -> icon; UI thread only
}  // namespace

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

Com<IDataObject> dataObject(HWND owner, const std::wstring& path) { return shellObject<IDataObject>(owner, path); }

Com<IContextMenu> itemMenu(HWND owner, const std::wstring& dir, const std::vector<Item>& items) {
    return shellObject<IContextMenu>(owner, dir, items);
}

Com<IContextMenu> itemMenu(HWND owner, const std::wstring& path) { return shellObject<IContextMenu>(owner, path); }

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

bool copyToClipboard(HWND owner, const std::wstring& dir, const std::vector<Item>& items, bool cut) {
    Com<IDataObject> obj = shellObject<IDataObject>(owner, dir, items);
    if (!obj) return false;
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
    return SUCCEEDED(OleSetClipboard(obj.get()));
}

void pasteInto(HWND owner, const std::wstring& dir, bool shortcuts) {
    if (dir.empty()) return;
    // Same code path as Explorer's Paste (and Paste shortcut), with Windows' progress UI.
    invokeVerb(owner, backgroundMenu(owner, dir).get(), shortcuts ? "pastelink" : "paste");
}

bool deleteItems(HWND owner, const std::wstring& dir, const std::vector<Item>& items, bool permanently) {
    Com<IDataObject> obj = shellObject<IDataObject>(owner, dir, items);
    if (!obj) return false;
    // FOF_WANTNUKEWARNING always: Windows still asks before deleting what it cannot recycle (a share, a USB stick).
    DWORD flags = FOF_WANTNUKEWARNING | (permanently ? 0 : FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION);
    return SUCCEEDED(fileOperation(owner, flags, [&](IFileOperation* op) { return op->DeleteItems(obj.get()); }));
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

void dragOut(HWND owner, const std::wstring& dir, const std::vector<Item>& items) {
    Com<IDataObject> obj = shellObject<IDataObject>(owner, dir, items);
    if (!obj) return;
    DWORD effect = 0;
    SHDoDragDrop(owner, obj.get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
}

std::optional<std::wstring> shortcutTarget(const std::wstring& lnkPath, bool* folder) {
    *folder = false;
    Com<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void**>(link.put()))))
        return std::nullopt;
    Com<IPersistFile> file = link.as<IPersistFile>();
    if (!file || FAILED(file->Load(lnkPath.c_str(), STGM_READ))) return std::nullopt;
    std::wstring target(32768, L'\0');   // not MAX_PATH: a folder deep in a network share has a longer path
    WIN32_FIND_DATAW stored{};            // the target as it was when the shortcut was made
    if (link->GetPath(target.data(), int(target.size()), &stored, 0) == S_OK && target[0]) {
        *folder = (stored.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return target.substr(0, target.find(L'\0'));
    }
    // No path: This PC is the one such target this app can show itself (as ""); Windows opens the rest.
    PIDLIST_ABSOLUTE p = nullptr;
    if (FAILED(link->GetIDList(&p)) || !p) return std::nullopt;
    Pidl pidl(p), pc = pidlFromPath(L"");
    if (!pc || !ILIsEqual(pidl.get(), pc.get())) return std::nullopt;
    *folder = true;
    return std::wstring();
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
        Com<IDropTarget> folder = std::exchange(folder_, {});   // it gets the drop, not a DragLeave
        finish();   // the drag is over (e.g. remove highlights)
        if (!folder || FAILED(folder->Drop(data, keys, pt, effect))) *effect = DROPEFFECT_NONE;
        return S_OK;
    }

private:
    // Aims the drop at the folder under the cursor; true if it changed (that folder has been told already).
    bool aim(DWORD keys, POINTL pt, DWORD* effect) {
        POINT p{pt.x, pt.y};
        std::optional<std::wstring> dir = target_(&p);
        if (dir == dir_) return false;
        if (folder_) folder_->DragLeave();
        folder_.reset();
        dir_ = dir;
        if (dir) uiObject(owner_, *dir, IID_IDropTarget, reinterpret_cast<void**>(folder_.put()));
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
