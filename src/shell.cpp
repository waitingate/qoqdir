// The Windows shell at work (shell.h): icons and types, menus, clipboard, drag and drop, opening things.
#include "shell.h"

#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <unordered_map>

// ---------------------------------------------------------------- shell objects

Pidl pidlFromPath(const std::wstring& path) {
    PIDLIST_ABSOLUTE p = nullptr;
    if (path.empty())
        SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &p);
    else
        SHParseDisplayName(path.c_str(), nullptr, &p, 0, nullptr);
    return Pidl(p);
}

namespace {

using ChildPidl = std::unique_ptr<std::remove_pointer_t<PITEMID_CHILD>, CoTaskMemDeleter>;

// An interface (menu, data object, drop target...) on one item by its full path; covers This PC and \\server\share.
HRESULT uiObject(HWND owner, const std::wstring& path, REFIID iid, void** out) {
    *out = nullptr;
    Pidl pidl = pidlFromPath(path);
    Com<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    if (!pidl || FAILED(SHBindToParent(pidl.get(), IID_PPV_ARGS(parent.put()), &child)))
        return E_FAIL;
    return parent->GetUIObjectOf(owner, 1, &child, iid, nullptr, out);
}

// IUnknown for an object of ours that serves the one interface I: counted, and gone with its last reference.
template <class I>
class Unknown : public I {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == __uuidof(I)) {
            *out = static_cast<I*>(this);
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

protected:
    virtual ~Unknown() = default;

private:
    LONG refs_ = 1;
};

// The listing's find data, handed to the shell (STR_FILE_SYS_BIND_DATA) so parsing a name needs no disk query.
class FindData final : public Unknown<IFileSystemBindData> {
public:
    HRESULT STDMETHODCALLTYPE SetFindData(const WIN32_FIND_DATAW* fd) override {
        fd_ = *fd;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFindData(WIN32_FIND_DATAW* fd) override {
        *fd = fd_;
        return S_OK;
    }

private:
    WIN32_FIND_DATAW fd_{};
};

// A bind context that hands the shell what 'known' holds. STGM_CREATE: take the find data as given.
// (RegisterObjectParam never writes its non-const key.)
Com<IBindCtx> findDataBind(Com<IFileSystemBindData>& known) {
    *known.put() = new FindData;
    Com<IBindCtx> bind;
    BIND_OPTS opts{};
    opts.cbStruct = sizeof opts;
    opts.grfMode = STGM_CREATE;
    if (FAILED(CreateBindCtx(0, bind.put())) || FAILED(bind->SetBindOptions(&opts)) ||
        FAILED(bind->RegisterObjectParam(const_cast<LPOLESTR>(STR_FILE_SYS_BIND_DATA), known.get())))
        return {};
    return bind;
}

// A folder as a shell folder, for its items or its background. The desktop itself (an empty pidl) can't be bound:
// it is the desktop folder.
Com<IShellFolder> shellFolder(const std::wstring& dir) {
    Com<IShellFolder> desktop, folder;
    Pidl pidl = pidlFromPath(dir);
    if (!pidl || FAILED(SHGetDesktopFolder(desktop.put()))) return folder;
    if (pidl->mkid.cb == 0) return desktop;
    desktop->BindToObject(pidl.get(), nullptr, IID_PPV_ARGS(folder.put()));
    return folder;
}

// The same for listed items of one folder: opened once, each item parsed from its own find data. Full
// paths took ~9 ms per file (90 s for 10,000), plain names a disk query each (spec 4.7).
HRESULT uiObject(HWND owner, const std::wstring& dir, const std::vector<Item>& items, REFIID iid, void** out) {
    *out = nullptr;
    if (items.empty()) return E_INVALIDARG;
    if (items.size() == 1) return uiObject(owner, joinPath(dir, items[0].name), iid, out);
    Com<IShellFolder> parent = shellFolder(dir);
    if (!parent) return E_FAIL;
    Com<IFileSystemBindData> known;
    Com<IBindCtx> bind = findDataBind(known);
    if (!bind) return E_FAIL;
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
        HRESULT hr = parent->ParseDisplayName(owner, bind.get(), fd.cFileName, nullptr, &child, nullptr);
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
    uiObject(owner, where..., IID_PPV_ARGS(obj.put()));
    return obj;
}

// Runs one IFileOperation: Windows shows progress, conflict and error dialogs, and records undo.
template <class AddWork>
HRESULT fileOperation(HWND owner, DWORD flags, AddWork addWork) {
    Com<IFileOperation> op;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(op.put()));
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
constexpr UINT kOwnFirst = 0x8000;   // the app's own items' ids, after the shell's
Com<IContextMenu2> g_menu2;   // set only while a context menu is on screen
Com<IContextMenu3> g_menu3;

}  // namespace

// ---------------------------------------------------------------- icons and types

HIMAGELIST systemImageList() {
    SHFILEINFOW sfi{};   // the list is the call's return value, not a field: the one use that fileInfo doesn't cover
    return reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(L"x", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof sfi,
        SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
}

// uxtheme.dll is loaded here, not imported: Windows looks for an imported DLL next to the exe first, before
// main() can restrict the search.
void setTheme(HWND control, const wchar_t* name) {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto set = reinterpret_cast<HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR)>(
        reinterpret_cast<void (*)()>(ux ? GetProcAddress(ux, "SetWindowTheme") : nullptr));
    if (set) set(control, name, *name ? nullptr : L"");   // "" for both: Windows' plain look
}

namespace {
struct TypeInfo {
    int icon = 0;
    std::shared_ptr<const std::wstring> type;   // given to every item of the type (Item::type)
};

std::wstring typeKey(const Item& it) { return it.isDir() ? L"\\" : extensionOf(it.name); }

// Icon and type name depend only on the extension: one shell call, with no disk access (but the type's icon handler).
TypeInfo askType(const Item& it) {
    std::wstring probe = it.isDir() ? L"folder" : L"file" + typeKey(it);
    SHFILEINFOW sfi = fileInfo(probe, it.isDir() ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
                               SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_TYPENAME)
                         .value_or(SHFILEINFOW{});
    return {sfi.iIcon, std::make_shared<const std::wstring>(sfi.szTypeName)};
}

// Cached for the UI thread: the folder icon, and the few items a load did not fill in (fillTypes). Not a local static:
// its guard against two threads alone links winpthreads in (35 KB).
std::unordered_map<std::wstring, TypeInfo> typeCache;

const TypeInfo& typeInfo(const Item& it) {
    auto [at, fresh] = typeCache.try_emplace(typeKey(it));
    if (fresh) at->second = askType(it);
    return at->second;
}
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
        if (auto sfi = fileInfo(joinPath(dir, name), 0, SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
            batch.icons.emplace_back(name, sfi->iIcon);
    }
    return batch;
}

int typeIcon(const Item& it) { return typeInfo(it).icon; }

void fillTypes(std::vector<Item>& items) {
    std::unordered_map<std::wstring, TypeInfo> seen;
    for (Item& it : items) {
        if (it.type) continue;   // a drive comes with its own
        auto [at, fresh] = seen.try_emplace(typeKey(it));
        if (fresh) at->second = askType(it);
        it.type = at->second.type;
        if (!hasOwnIcon(it)) it.icon = at->second.icon;
    }
}

int folderIcon() {
    Item dir;
    dir.attrs = FILE_ATTRIBUTE_DIRECTORY;
    return typeInfo(dir).icon;
}

std::shared_ptr<const std::wstring> sharedTypeName(const Item& it) { return it.type ? it.type : typeInfo(it).type; }

const std::wstring& typeName(const Item& it) { return it.type ? *it.type : *typeInfo(it).type; }

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    HRESULT hr = SHGetKnownFolderPath(id, 0, nullptr, &p);
    std::unique_ptr<wchar_t, CoTaskMemDeleter> owned(p);   // freed even when the call fails
    return SUCCEEDED(hr) ? p : L"";
}

// ---------------------------------------------------------------- menus

Com<IDataObject> dataObject(HWND owner, const std::wstring& path) { return shellObject<IDataObject>(owner, path); }

Com<IContextMenu> itemMenu(HWND owner, const std::wstring& dir, const std::vector<Item>& items) {
    return shellObject<IContextMenu>(owner, dir, items);
}

Com<IContextMenu> itemMenu(HWND owner, const std::wstring& path) { return shellObject<IContextMenu>(owner, path); }

Com<IContextMenu> backgroundMenu(HWND owner, const std::wstring& dir) {
    Com<IContextMenu> cm;
    if (Com<IShellFolder> folder = shellFolder(dir))
        folder->CreateViewObject(owner, IID_PPV_ARGS(cm.put()));
    return cm;
}

void showMenu(HWND owner, IContextMenu* cm, POINT pt, const std::function<bool(const std::wstring&)>& intercept,
              const std::vector<OwnItem>& own) {
    if (!cm) return;
    Menu menu(CreatePopupMenu());
    if (!menu) return;
    for (size_t i = 0; i < own.size(); ++i)
        AppendMenuW(menu.get(), MF_STRING | (own[i].enabled ? 0 : MF_GRAYED), kOwnFirst + i, own[i].text);
    if (!own.empty()) AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    UINT flags = CMF_NORMAL | CMF_CANRENAME;
    if (GetKeyState(VK_SHIFT) < 0) flags |= CMF_EXTENDEDVERBS;
    if (FAILED(cm->QueryContextMenu(menu.get(), UINT(GetMenuItemCount(menu.get())), kFirstCmd, kLastCmd, flags))) return;

    cm->QueryInterface(IID_PPV_ARGS(g_menu2.put()));
    cm->QueryInterface(IID_PPV_ARGS(g_menu3.put()));
    UINT id = UINT(TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, owner, nullptr));
    g_menu2.reset();
    g_menu3.reset();
    if (id >= kOwnFirst && id - kOwnFirst < own.size()) return own[id - kOwnFirst].act();
    if (id < kFirstCmd) return;

    wchar_t verb[80] = {};
    if (FAILED(cm->GetCommandString(id - kFirstCmd, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb), 80)))
        verb[0] = 0;
    verb[79] = 0;   // a handler that fills all 80 without a terminator must not run intercept()'s wcslen off the end
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
    *result = 0;
    if (g_menu3) return SUCCEEDED(g_menu3->HandleMenuMsg2(msg, wp, lp, result));
    return g_menu2 && SUCCEEDED(g_menu2->HandleMenuMsg(msg, wp, lp));
}

void invokeVerb(HWND owner, IContextMenu* cm, const char* verb) {
    if (!cm) return;
    Menu menu(CreatePopupMenu());
    if (!menu || FAILED(cm->QueryContextMenu(menu.get(), 0, kFirstCmd, kLastCmd, CMF_NORMAL))) return;
    std::wstring verbW(verb, verb + strlen(verb));
    CMINVOKECOMMANDINFOEX ici{};
    ici.cbSize = sizeof ici;
    ici.fMask = CMIC_MASK_UNICODE;
    ici.hwnd = owner;
    ici.lpVerb = verb;
    ici.lpVerbW = verbW.c_str();
    ici.nShow = SW_SHOWNORMAL;
    cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ici));
}

// ---------------------------------------------------------------- file operations

namespace {

// A copy of the bytes, as a data object or the clipboard takes them; null if there is no room.
HGLOBAL globalCopy(const void* bytes, size_t size) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, size);
    void* p = h ? GlobalLock(h) : nullptr;
    if (!p) {
        if (h) GlobalFree(h);
        return nullptr;
    }
    CopyMemory(p, bytes, size);
    GlobalUnlock(h);
    return h;
}

// Puts the bytes into a data object as 'format', which then owns them; false if it took nothing.
bool setData(IDataObject* data, UINT format, const void* bytes, size_t size) {
    FORMATETC fmt{CLIPFORMAT(format), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM med{};
    med.tymed = TYMED_HGLOBAL;
    med.hGlobal = globalCopy(bytes, size);
    if (!med.hGlobal) return false;
    if (SUCCEEDED(data->SetData(&fmt, &med, TRUE))) return true;
    ReleaseStgMedium(&med);
    return false;
}

}  // namespace

bool copyToClipboard(HWND owner, const std::wstring& dir, const std::vector<Item>& items, bool cut) {
    Com<IDataObject> obj = shellObject<IDataObject>(owner, dir, items);
    if (!obj) return false;
    // Tells the paste target whether to move (cut) or copy; a copy also allows Paste shortcut.
    DWORD effect = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY | DROPEFFECT_LINK;
    setData(obj.get(), RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), &effect, sizeof(effect));
    return SUCCEEDED(OleSetClipboard(obj.get()));
}

void pasteInto(HWND owner, const std::wstring& dir, bool shortcuts) {
    if (dir.empty()) return;
    // Same code path as Explorer's Paste (and Paste shortcut), with Windows' progress UI.
    invokeVerb(owner, backgroundMenu(owner, dir).get(), shortcuts ? "pastelink" : "paste");
}

bool canPaste() {
    // Files, or items with no file path that Windows' Paste takes too: from inside a zip, on a phone, or attachments
    // another program offers as files.
    for (UINT format : {UINT(CF_HDROP), RegisterClipboardFormatW(CFSTR_SHELLIDLIST),
                        RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)})
        if (IsClipboardFormatAvailable(format)) return true;
    return false;
}

bool deleteItems(HWND owner, const std::wstring& dir, const std::vector<Item>& items, bool permanently) {
    Com<IDataObject> obj = shellObject<IDataObject>(owner, dir, items);
    if (!obj) return false;
    // FOF_WANTNUKEWARNING always: Windows still asks before deleting what it cannot recycle (a share, a USB stick).
    DWORD flags = FOF_WANTNUKEWARNING | (permanently ? 0 : FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION);
    return SUCCEEDED(fileOperation(owner, flags, [&](IFileOperation* op) { return op->DeleteItems(obj.get()); }));
}

std::optional<std::wstring> renameItem(HWND owner, const std::wstring& path, const std::wstring& newName) {
    Pidl pidl = pidlFromPath(path);
    Com<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    if (!pidl || FAILED(SHBindToParent(pidl.get(), IID_PPV_ARGS(parent.put()), &child))) return std::nullopt;
    constexpr SHGDNF literal = SHGDN_INFOLDER | SHGDN_FORPARSING;   // as typed, whether Explorer hides extensions or not
    PITEMID_CHILD renamed = nullptr;
    if (FAILED(parent->SetNameOf(owner, child, newName.c_str(), literal, &renamed))) return std::nullopt;
    ChildPidl made(renamed);
    STRRET name{};
    wchar_t text[MAX_PATH];
    if (!made || FAILED(parent->GetDisplayNameOf(made.get(), literal, &name)) ||
        FAILED(StrRetToBufW(&name, made.get(), text, MAX_PATH)))
        return newName;   // the name asked for if Windows said nothing
    return text;
}

namespace {
// What a worker asks of Windows: no box, and NOASYNC, as it ends right after.
constexpr ULONG kFromWorker = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;

bool shellExecute(HWND owner, ULONG mask, const wchar_t* verb, const wchar_t* file, const wchar_t* args,
                  const wchar_t* dir) {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = mask;
    sei.hwnd = owner;
    sei.lpVerb = verb;
    sei.lpFile = file;
    sei.lpParameters = args;
    sei.lpDirectory = dir;
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}
}  // namespace

void openFile(HWND owner, const std::wstring& path) {
    // Windows shows its own "open with" / error dialog.
    shellExecute(owner, 0, nullptr, path.c_str(), nullptr, parentPath(path).c_str());
}

namespace {
// `text` as a part of a URL: UTF-8, everything but letters, digits and -_.~ as %XX, the way search-ms: links are
// written ("C%3A%5CUsers").
std::wstring urlPart(const std::wstring& text) {
    std::string utf8 = toUtf8(text);
    std::wstring out;
    for (unsigned char c : utf8) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += wchar_t(c);
        else out += L'%' + std::wstring{L"0123456789ABCDEF"[c >> 4], L"0123456789ABCDEF"[c & 15]};
    }
    return out;
}
}  // namespace

void openInExplorer(HWND owner, const std::wstring& dir, bool search) {
    std::wstring file = search ? L"search-ms:crumb=location:" + urlPart(dir) : dir;
    shellExecute(owner, 0, search ? nullptr : L"explore", file.c_str(), nullptr, nullptr);
}

namespace {
// The full path App Paths gives for a program name ("chrome", "winword"), as the Run box finds one there, or ""
// for none. The key is the name with ".exe" when it carries no extension of its own.
std::wstring appPathProgram(const std::wstring& name) {
    std::wstring key = extensionOf(name).empty() ? name + L".exe" : name;
    wchar_t out[1024];
    DWORD n = DWORD(std::size(out));
    return SUCCEEDED(AssocQueryStringW(ASSOCF_OPEN_BYEXENAME, ASSOCSTR_EXECUTABLE, key.c_str(), nullptr, out, &n))
               ? out
               : L"";
}
}  // namespace

// The full path a bare name typed in the address bar runs, found the way the Run box finds one: the PATH, then
// App Paths. Windows would also look in the folder shown, where a program of that name could be waiting (in
// Downloads, on a USB stick), so a program name found in neither place comes back as nothing and is not run from
// there (5.3). A path or a web address (it has a slash or colon), and a document name (an extension outside
// PATHEXT, "notes.txt"), stay as typed: a document still opens the folder shown's own.
std::optional<std::wstring> commandProgram(const std::wstring& name) {
    if (name.find_first_of(L"\\/:") != std::wstring::npos) return name;
    std::wstring own = extensionOf(name);
    std::wstring exts = toLower(expandVars(L"%PATHEXT%"));
    if (exts.find(L'%') != std::wstring::npos) exts = L".com;.exe;.bat;.cmd";   // unset: the usual few
    exts += L';';
    if (!own.empty() && exts.find(own + L';') == std::wstring::npos) return name;   // a document, not a program
    // Folder by folder along the PATH, each tried with every extension, as cmd and the Run box go: a .cmd early on the
    // PATH wins over an .exe of that name further on.
    std::wstring path = expandVars(L"%PATH%"), found(32768, L'\0');
    for (size_t from = 0, semi; from <= path.size(); from = semi + 1) {
        semi = std::min(path.find(L';', from), path.size());
        std::wstring dir = path.substr(from, semi - from);
        if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"') dir = dir.substr(1, dir.size() - 2);
        if (dir.empty()) continue;
        for (size_t at = 0, end; (end = exts.find(L';', at)) != std::wstring::npos; at = end + 1) {
            std::wstring ext = exts.substr(at, end - at);
            if (ext.empty() || (!own.empty() && ext != own)) continue;
            DWORD n = SearchPathW(dir.c_str(), name.c_str(), ext.c_str(), DWORD(found.size()), found.data(), nullptr);
            if (n > 0 && n < found.size()) return found.substr(0, n);
        }
    }
    std::wstring app = appPathProgram(name);
    if (!app.empty()) return app;
    return std::nullopt;   // on neither PATH nor App Paths: not the folder shown's own file (5.3)
}

bool runCommand(HWND owner, const std::wstring& command, const std::wstring& workDir) {
    const wchar_t* dir = workDir.empty() ? nullptr : workDir.c_str();
    auto run = [&](std::optional<std::wstring> file, const wchar_t* args) {
        if (!file) return false;   // a program name on neither PATH nor App Paths: never run from the folder (5.3)
        return shellExecute(owner, kFromWorker, nullptr, file->c_str(), args, dir);
    };
    if (run(commandProgram(command), nullptr)) return true;
    // "notepad file.txt": program first, the rest as arguments.
    size_t space = command.find(L' ');
    if (space == std::wstring::npos) return false;
    std::wstring args = command.substr(space + 1);
    return run(commandProgram(command.substr(0, space)), args.c_str());
}

std::optional<Terminal> terminalProgram() {
    if (std::optional<std::wstring> wt = commandProgram(L"wt")) return Terminal{*wt, L"-d ."};   // "." is its folder
    if (std::optional<std::wstring> cmd = commandProgram(L"cmd")) return Terminal{*cmd, L""};
    return std::nullopt;
}

bool startTerminal(HWND owner, const Terminal& terminal, const std::wstring& dir) {
    const wchar_t* args = terminal.args.empty() ? nullptr : terminal.args.c_str();
    return shellExecute(owner, kFromWorker, nullptr, terminal.program.c_str(), args, dir.c_str());
}

bool copyText(HWND owner, const std::wstring& text) {
    HGLOBAL h = globalCopy(text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    if (!h) return false;
    bool ok = OpenClipboard(owner);   // another program can hold it for a moment: then it fails, as Ctrl+C would
    if (ok) {
        ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, h);
        CloseClipboard();
    }
    if (!ok) GlobalFree(h);   // once set, the clipboard owns it
    return ok;
}

void dragOut(HWND owner, const std::wstring& dir, const std::vector<Item>& items) {
    Com<IDataObject> obj = shellObject<IDataObject>(owner, dir, items);
    if (!obj) return;
    DWORD effect = 0;
    SHDoDragDrop(owner, obj.get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
}

// ---------------------------------------------------------------- shortcuts

std::optional<std::wstring> shortcutTarget(const std::wstring& lnkPath, bool* folder) {
    *folder = false;
    if (extensionOf(lnkPath) != L".lnk") return std::nullopt;
    Com<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put()))))
        return std::nullopt;
    Com<IPersistFile> file = link.as<IPersistFile>();
    if (!file || FAILED(file->Load(lnkPath.c_str(), STGM_READ))) return std::nullopt;
    std::wstring target(32768, L'\0');   // not MAX_PATH: a folder deep in a network share has a longer path
    WIN32_FIND_DATAW stored{};            // the target as it was when the shortcut was made
    if (link->GetPath(target.data(), int(target.size()), &stored, 0) == S_OK && target[0]) {
        *folder = (stored.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        target.resize(target.find(L'\0'));
        // A target on a drive letter that is gone (a flash drive that came back under another letter) is looked for
        // where the shortcut says it is relative to itself: Windows stores that for one made on the target's drive.
        // No search, no box, and no disk is asked: a drive that is there is left to the loader, since a sleeping
        // disk or a server that is off would freeze the UI thread (a drag over the tree asks here).
        bool onDrive = target.size() >= 3 && target[1] == L':';
        if (onDrive && GetDriveTypeW(target.substr(0, 3).c_str()) == DRIVE_NO_ROOT_DIR &&
            link->Resolve(nullptr, SLR_NO_UI | SLR_NOSEARCH | SLR_NOTRACK | SLR_NOLINKINFO | SLR_NOUPDATE) == S_OK) {
            std::wstring found(32768, L'\0');
            if (link->GetPath(found.data(), int(found.size()), nullptr, 0) == S_OK) target = found.c_str();
        }
        return normalizePath(target);
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
// A shortcut to `folder` ("" for This PC) at `lnk`, made from the path and find data that say "folder": the
// shortcut then says so too, even for a folder that can't be reached now.
bool writeFolderShortcut(const std::wstring& lnk, const std::wstring& folder) {
    Com<IFileSystemBindData> known;
    Com<IBindCtx> bind = findDataBind(known);
    WIN32_FIND_DATAW fd{};
    fd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    leafName(folder).copy(fd.cFileName, MAX_PATH - 1);
    if (bind) known->SetFindData(&fd);
    PIDLIST_ABSOLUTE p = nullptr;
    if (!folder.empty() && bind) SHParseDisplayName(folder.c_str(), bind.get(), &p, 0, nullptr);
    Pidl pidl = folder.empty() ? pidlFromPath(folder) : Pidl(p);
    Com<IShellLinkW> link;
    Com<IPersistFile> file;
    return pidl &&
           SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put()))) &&
           SUCCEEDED(link->SetIDList(pidl.get())) && (file = link.as<IPersistFile>()) &&
           SUCCEEDED(file->Save(lnk.c_str(), TRUE));
}

// The shortcut's file name for a folder: its display name, without what a file name can't hold.
std::wstring shortcutName(const std::wstring& folder) {
    std::wstring name = displayName(folder);
    std::erase_if(name, [](wchar_t c) { return std::wstring_view(L"\\/:*?\"<>|").find(c) != std::wstring_view::npos; });
    return name + L".lnk";
}
}  // namespace

bool saveFolderShortcuts(const std::wstring& dir, const std::vector<std::wstring>& folders) {
    // One save at a time: closing the window saves here and now while Ctrl+S's worker may still be writing (a folder
    // on a server that is off is slow), and the two would leave a mix of both sets. The later one waits and wins.
    // Windows' own lock: std::mutex links winpthreads in (35 KB).
    static SRWLOCK saving = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&saving);
    struct Release {
        ~Release() { ReleaseSRWLockExclusive(&saving); }
    } one;
    CreateDirectoryW(dir.c_str(), nullptr);   // fails harmlessly if it exists
    for (const Item& it : listFolder(dir, true).items)
        if (!it.isDir() && extensionOf(it.name) == L".lnk") DeleteFileW(joinPath(dir, it.name).c_str());
    bool ok = true;
    for (size_t i = 0; i < folders.size(); ++i)
        ok &= writeFolderShortcut(joinPath(dir, std::to_wstring(i + 1) + L" " + shortcutName(folders[i])), folders[i]);
    return ok;
}

bool bookmarkFolder(const std::wstring& dir, const std::wstring& folder) {
    // The shortcut's name comes from the folder's display name, so two folders can want the same one (D:\work\logs
    // and E:\logs both "logs.lnk"). Keep an existing one that already points here (bookmarking again), but give a
    // different folder the next free "name (2).lnk", as New folder numbers a clash, rather than claim it did.
    std::wstring base = shortcutName(folder);   // "<name>.lnk"
    std::wstring stem = base.substr(0, base.size() - 4);
    for (int n = 1; n <= 99; ++n) {
        std::wstring lnk = joinPath(dir, n == 1 ? base : stem + L" (" + std::to_wstring(n) + L").lnk");
        if (!exists(lnk)) return writeFolderShortcut(lnk, folder);
        bool isFolder = false;
        std::optional<std::wstring> target = shortcutTarget(lnk, &isFolder);
        if (isFolder && target && samePath(*target, folder)) return true;   // already bookmarked
    }
    return false;
}

std::vector<std::wstring> savedFolders(const std::wstring& dir) {
    Listing listing = listFolder(dir, true);
    sortItems(listing.items, SortKey::Name, true);
    std::vector<std::wstring> folders;
    for (const Item& it : listing.items) {
        bool folder = false;
        std::optional<std::wstring> target = shortcutTarget(joinPath(dir, it.name), &folder);
        if (target && folder) folders.push_back(*target);
    }
    return folders;
}

// ---------------------------------------------------------------- background work

namespace {
DWORD WINAPI worker(void* job) {
    std::unique_ptr<std::function<void()>> work(static_cast<std::function<void()>*>(job));
    ULONG guard = 64 * 1024;   // room for the crash log if this worker overflows its stack (as the main thread has)
    SetThreadStackGuarantee(&guard);
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    (*work)();
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}
}  // namespace

// A Windows thread, not a std::thread: that goes through winpthreads, which kept a handle for each detached thread
// in its mingw-w64 14 builds (4.10).
void inBackground(std::function<void()> work) {
    auto job = std::make_unique<std::function<void()>>(std::move(work));
    HANDLE t = CreateThread(nullptr, 0, worker, job.get(), 0, nullptr);
    if (!t) abort();   // as std::thread did (no exceptions): the reply its caller waits for would never come
    job.release();   // the worker owns it now
    CloseHandle(t);
}

// ---------------------------------------------------------------- folder watch

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
    if (ids_[0]) dir_ = dir;
}

FolderWatch::~FolderWatch() {
    for (ULONG id : ids_)
        if (id) SHChangeNotifyDeregister(id);
}

FolderWatch::FolderWatch(FolderWatch&& o) noexcept {
    std::swap(ids_, o.ids_);
    std::swap(dir_, o.dir_);
}

FolderWatch& FolderWatch::operator=(FolderWatch&& o) noexcept {
    std::swap(ids_, o.ids_);
    std::swap(dir_, o.dir_);
    return *this;
}

void acknowledgeChange(WPARAM wp, LPARAM lp) {
    HANDLE lock = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wp), DWORD(lp), nullptr, nullptr);
    if (lock) SHChangeNotification_Unlock(lock);
}

// ---------------------------------------------------------------- drag and drop

std::optional<std::wstring> localFolder(std::wstring path, bool shortcut) {
    bool folder = true;
    if (shortcut) path = shortcutTarget(path, &folder).value_or(L"");
    bool local = folder && path.size() >= 3 && path[1] == L':' &&
                 GetDriveTypeW(path.substr(0, 3).c_str()) != DRIVE_REMOTE;
    // A shortcut still says folder when its folder is gone: only looking tells, which costs no more than the drop
    // target does, as it reads that folder next.
    if (local && shortcut) local = isDirectory(path);
    return local ? std::optional(path) : std::nullopt;
}

namespace {

// The paths of the files a drag carries, read once as it enters. None for items without one (from a zip folder or a
// phone), and none for a drag that can only make shortcuts (a drive from This PC): a shortcut may go anywhere.
std::vector<std::wstring> draggedPaths(IDataObject* data, DWORD allowed) {
    std::vector<std::wstring> paths;
    FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM med{};
    if (!data || !(allowed & (DROPEFFECT_COPY | DROPEFFECT_MOVE)) || FAILED(data->GetData(&fmt, &med))) return paths;
    // The names one after another, read straight (DragQueryFileW(i) starts from the first name each time), and no
    // further than the memory goes: another program wrote them.
    size_t size = GlobalSize(med.hGlobal);
    if (auto* drop = size > sizeof(DROPFILES) ? static_cast<const char*>(GlobalLock(med.hGlobal)) : nullptr) {
        auto* files = reinterpret_cast<const DROPFILES*>(drop);
        if (files->pFiles < size) {
            const char* list = drop + files->pFiles;
            size_t bytes = size - files->pFiles;
            // An older program's drag has them in ANSI, in the code page: made UTF-16 in one go, nuls and all.
            std::wstring ansi;
            if (!files->fWide) {
                int n = int(std::min(bytes, size_t(0x7FFFFFFF)));
                ansi.resize(size_t(std::max(MultiByteToWideChar(CP_ACP, 0, list, n, nullptr, 0), 0)));
                MultiByteToWideChar(CP_ACP, 0, list, n, ansi.data(), int(ansi.size()));
            }
            const wchar_t* p = files->fWide ? reinterpret_cast<const wchar_t*>(list) : ansi.data();
            const wchar_t* end = p + (files->fWide ? bytes / sizeof(wchar_t) : ansi.size());
            for (const wchar_t* name = p; p < end; name = ++p) {
                while (p < end && *p) ++p;
                if (p == end || p == name) break;   // cut short, or the empty name that ends the list
                paths.push_back(normalizePath(std::wstring(name, p)));
            }
        }
        GlobalUnlock(med.hGlobal);
    }
    ReleaseStgMedium(&med);
    return paths;
}

class DropTarget final : public Unknown<IDropTarget> {
public:
    DropTarget(HWND owner, DropTargetFn target) : owner_(owner), target_(std::move(target)) {
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(helper_.put()));
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
        if (data) data->AddRef();
        *data_.put() = data;
        DWORD allowed = *effect;
        dragged_ = draggedPaths(data, allowed);
        if (!aim(keys, pt, effect)) *effect = DROPEFFECT_NONE;   // still no folder (each drag starts with none)
        describe(asks(keys, allowed), *effect);
        POINT p{pt.x, pt.y};
        if (helper_) helper_->DragEnter(owner_, data, &p, *effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override {
        bool ask = asks(keys, *effect);
        if (!ask) clearTip();   // ours goes first: the folder puts its own
        // Over a new folder, aim() has already asked it; otherwise the same folder decides again.
        if (!aim(keys, pt, effect) && (!folder_ || FAILED(folder_->DragOver(dropKeys(keys, *effect), pt, effect))))
            *effect = DROPEFFECT_NONE;
        describe(ask, *effect);
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
        if (!folder || FAILED(folder->Drop(data, dropKeys(keys, *effect), pt, effect))) *effect = DROPEFFECT_NONE;
        return S_OK;
    }

private:
    // Aims the drop at the folder under the cursor; true if it changed (that folder has been told already).
    bool aim(DWORD keys, POINTL pt, DWORD* effect) {
        POINT p{pt.x, pt.y};
        std::optional<std::wstring> dir = target_(&p, dragged_);
        // The same folder as at the last move changes nothing, and dropBack walks all the paths dragged: with
        // 10,000, once for every move of the mouse was too slow.
        if (dir == asked_) return false;
        asked_ = dir;
        if (dir && dropBack(*dir, dragged_)) dir.reset();   // never back where the files are, whoever asks (4.8)
        if (dir == dir_) return false;
        if (folder_) folder_->DragLeave();
        folder_.reset();
        dir_ = dir;
        if (dir) folder_ = shellObject<IDropTarget>(owner_, *dir);
        // A plain drag that could move files reaches the folder as a right-button drag: Windows asks (5.8).
        if (folder_ && FAILED(folder_->DragEnter(data_.get(), dropKeys(keys, *effect), pt, effect))) folder_.reset();
        if (!folder_) *effect = DROPEFFECT_NONE;
        return true;
    }
    void finish() {
        target_(nullptr, {});
        if (folder_) folder_->DragLeave();
        folder_.reset();
        asked_.reset();
        dir_.reset();
        clearTip();
        data_.reset();
        dragged_.clear();
    }

    // A drop with these keys asks with Windows' menu, and could move the files: a plain drag passed on as a
    // right-button one (dropKeys), or a right-drag (5.8).
    static bool asks(DWORD keys, DWORD allowed) {
        return (dropKeys(keys, allowed) & MK_RBUTTON) && (allowed & DROPEFFECT_MOVE);
    }
    // While a drop would ask, the drag's tip says "Copy or move to" its folder, not the folder's own "Move to", and
    // shows no icon: none of Windows' icons says that a menu comes (5.8).
    void describe(bool ask, DWORD effect) {
        if (folder_ && dir_ && effect != DROPEFFECT_NONE && ask)
            setTip(DROPIMAGE_NOIMAGE, L"Copy or move to %1", displayName(*dir_));
        else
            clearTip();
    }
    void clearTip() {   // takes ours away, so the folder's own shows again
        if (described_) setTip(DROPIMAGE_INVALID);
    }
    // Puts a tip in the drag's data for its image to show; DROPIMAGE_INVALID takes ours away.
    void setTip(DROPIMAGETYPE type, const wchar_t* message = L"", const std::wstring& insert = L"") {
        DROPDESCRIPTION tip{};
        tip.type = type;
        lstrcpynW(tip.szMessage, message, MAX_PATH);
        lstrcpynW(tip.szInsert, insert.c_str(), MAX_PATH);
        UINT format = RegisterClipboardFormatW(CFSTR_DROPDESCRIPTION);
        described_ = data_ && setData(data_.get(), format, &tip, sizeof(tip)) && type != DROPIMAGE_INVALID;
    }

    HWND owner_;
    DropTargetFn target_;
    Com<IDataObject> data_;
    std::vector<std::wstring> dragged_;   // the paths of the files it carries (draggedPaths)
    std::optional<std::wstring> asked_;   // the folder target_ gave last, before dropBack (aim)
    std::optional<std::wstring> dir_;     // the folder the drop currently goes to
    Com<IDropTarget> folder_;             // and its shell drop target
    Com<IDropTargetHelper> helper_;
    bool described_ = false;              // the drag's tip is ours (describe)
};

}  // namespace

Com<IDropTarget> newDropTarget(HWND owner, DropTargetFn target) {
    Com<IDropTarget> t;
    *t.put() = new DropTarget(owner, std::move(target));
    return t;
}

// ---------------------------------------------------------------- shell process

Com<IUnknown> holdProcess() {
    Com<IUnknown> hold;
    SHGetInstanceExplorer(hold.put());   // the ShellProcessRef main made, as Windows' own copies take it
    return hold;
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
