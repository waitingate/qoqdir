#include "tree.h"

#include "shell.h"

#include <uxtheme.h>

#include <thread>

namespace {

// Subfolders of `dir` ("" = the drives), and its shortcuts if asked, into `batch`. Worker thread: may block.
void readChildren(TreeBatch& batch, const std::wstring& dir, bool shortcuts) {
    std::vector<TreeBatch::Child>& children = batch.children;
    if (dir.empty()) {   // the drives (not via listFolder: its free-space check can wait on a network drive)
        DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (!(mask & (1u << i))) continue;
            std::wstring root = std::wstring(1, wchar_t(L'A' + i)) + L":\\";
            SHFILEINFOW sfi{};
            SHGetFileInfoW(root.c_str(), 0, &sfi, sizeof sfi, SHGFI_DISPLAYNAME | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
            children.push_back({root, sfi.szDisplayName[0] ? sfi.szDisplayName : root.substr(0, 2), sfi.iIcon});
        }
        return;
    }
    Listing listing = listFolder(dir, false);
    batch.failed = listing.error != 0;
    sortItems(listing.items, SortKey::Name, true);   // folders first, in natural order
    for (const Item& it : listing.items) {
        std::wstring path = joinPath(dir, it.name);
        if (it.isDir()) {
            children.push_back({path, it.name, -1});
        } else if (shortcuts && isShortcut(it.name)) {
            // From the .lnk alone: its name and a folder or file-type icon. Its own icon can wait on a server
            // that is off, so it follows later (startWorker).
            bool folder = false;
            std::optional<std::wstring> target = shortcutTarget(path, &folder);
            SHFILEINFOW name{}, icon{};
            SHGetFileInfoW(path.c_str(), 0, &name, sizeof name, SHGFI_DISPLAYNAME);
            std::wstring probe = folder ? L"x" : target.value_or(it.name);   // only the extension matters
            SHGetFileInfoW(probe.c_str(), folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &icon, sizeof icon,
                           SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
            children.push_back({path, name.szDisplayName[0] ? name.szDisplayName : it.name, icon.iIcon, true});
        }
    }
}

// The user folders that exist, as roots, into `batch`. Worker thread: one on a dead server can take 20-60 s.
void readUserFolders(TreeBatch& batch) {
    const KNOWNFOLDERID* folders[] = {&FOLDERID_Desktop, &FOLDERID_Downloads, &FOLDERID_Documents,
                                      &FOLDERID_Pictures, &FOLDERID_Music,    &FOLDERID_Videos};
    for (const KNOWNFOLDERID* id : folders) {
        std::wstring path = knownFolder(*id);
        SHFILEINFOW sfi{};
        if (path.empty() || !SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof sfi,
                                            SHGFI_DISPLAYNAME | SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
            continue;
        batch.children.push_back({path, sfi.szDisplayName, sfi.iIcon});
    }
}

}  // namespace

void FolderTree::create(HWND parent, int id, UINT batchMsg, UINT iconMsg, const std::wstring& bookmarks) {
    parent_ = parent;
    bookmarks_ = bookmarks;
    batchMsg_ = batchMsg;
    iconMsg_ = iconMsg;
    // TVS_LINESATROOT gives the roots an expand arrow; without TVS_HASLINES no lines show, as in Explorer.
    tv_ = CreateWindowExW(0, WC_TREEVIEWW, L"",
                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS |
                              TVS_TRACKSELECT | TVS_FULLROWSELECT | TVS_NOHSCROLL,
                          0, 0, 0, 0, parent, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    TreeView_SetExtendedStyle(tv_, TVS_EX_DOUBLEBUFFER | TVS_EX_FADEINOUTEXPANDOS,
                              TVS_EX_DOUBLEBUFFER | TVS_EX_FADEINOUTEXPANDOS);
    SetWindowTheme(tv_, L"Explorer", nullptr);
    TreeView_SetImageList(tv_, systemImageList(), TVSIL_NORMAL);
}

void FolderTree::fill() {
    reading_.clear();   // results still being read for the old nodes will be dropped
    TreeView_DeleteAllItems(tv_);   // forgets each node's path too (TVN_DELETEITEM)
    HTREEITEM bookmarks = add(TVI_ROOT, bookmarks_, L"Bookmarks", folderIcon());
    startWorker(nullptr, readUserFolders);   // the user folders arrive later, like the drives
    SHFILEINFOW pc{};
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &pidl))) {
        SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &pc, sizeof pc, SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        CoTaskMemFree(pidl);
    }
    HTREEITEM thisPc = add(TVI_ROOT, L"", L"This PC", pc.iIcon);
    TreeView_Expand(tv_, bookmarks, TVE_EXPAND);
    TreeView_Expand(tv_, thisPc, TVE_EXPAND);
}

// The shortcuts' own icons, read after their node was shown (startWorker).
void FolderTree::setIcons(const IconBatch& batch) {
    if (!samePath(batch.dir, bookmarks_)) return;   // shortcuts are shown only under Bookmarks
    for (HTREEITEM c = TreeView_GetChild(tv_, TreeView_GetRoot(tv_)); c; c = TreeView_GetNextSibling(tv_, c)) {
        for (const auto& [name, icon] : batch.icons) {
            if (!samePath(pathOf(c), joinPath(batch.dir, name))) continue;
            TVITEMW item{};
            item.mask = TVIF_IMAGE | TVIF_SELECTEDIMAGE;
            item.hItem = c;
            item.iImage = item.iSelectedImage = icon;
            TreeView_SetItem(tv_, &item);
        }
    }
}

// Drives came or went: re-read This PC only, so expanded folders stay open (4.5); its read in flight is dropped.
void FolderTree::refreshDrives() {
    HTREEITEM pc = TreeView_GetRoot(tv_);
    for (HTREEITEM n = pc; n; n = TreeView_GetNextSibling(tv_, n)) pc = n;   // This PC is the last root
    bool expanded = (TreeView_GetItemState(tv_, pc, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
    reading_.erase(pc);
    TreeView_Expand(tv_, pc, TVE_COLLAPSE | TVE_COLLAPSERESET);   // forget the drives; the next expand reads them
    if (expanded) TreeView_Expand(tv_, pc, TVE_EXPAND);
}

HTREEITEM FolderTree::add(HTREEITEM parent, const std::wstring& path, const std::wstring& text, int icon, bool leaf,
                          HTREEITEM after) {
    TVINSERTSTRUCTW ins{};
    ins.hParent = parent;
    ins.hInsertAfter = after;
    ins.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
    ins.item.pszText = const_cast<wchar_t*>(text.c_str());
    ins.item.iImage = ins.item.iSelectedImage = icon;
    ins.item.cChildren = leaf ? 0 : 1;   // a folder's are unknown until expanded
    HTREEITEM node = TreeView_InsertItem(tv_, &ins);
    if (node) paths_[node] = path;
    return node;
}

const std::wstring& FolderTree::pathOf(HTREEITEM node) const {
    return paths_.at(node);   // a node that is not there is a dead handle: stop loudly (4.10)
}

void FolderTree::startReading(HTREEITEM node) {
    if (TreeView_GetChild(tv_, node) || reading_.count(node)) return;   // already read, or being read
    std::wstring dir = pathOf(node);
    bool shortcuts = node == TreeView_GetRoot(tv_);   // the Bookmarks node
    startWorker(node, [dir, shortcuts](TreeBatch& batch) { readChildren(batch, dir, shortcuts); });
}

void FolderTree::startWorker(HTREEITEM node, std::function<void(TreeBatch&)> reader) {
    unsigned read = reading_[node] = ++reads_;
    HWND parent = parent_;
    UINT msg = batchMsg_, iconMsg = iconMsg_;
    std::thread([parent, msg, iconMsg, read, node, reader = std::move(reader)] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        auto batch = std::make_unique<TreeBatch>();
        batch->read = read;
        batch->node = node;
        reader(*batch);
        std::wstring dir;
        std::vector<std::wstring> shortcuts;
        for (const auto& c : batch->children) {
            if (!c.shortcut) continue;
            dir = parentPath(c.path);
            shortcuts.push_back(leafName(c.path));
        }
        if (PostMessageW(parent, msg, 0, reinterpret_cast<LPARAM>(batch.get()))) batch.release();
        // Then each shortcut's own icon, which asks the target (maybe a server that is off), as the list does.
        if (!shortcuts.empty()) {
            auto icons = std::make_unique<IconBatch>(readOwnIcons(dir, shortcuts));
            if (PostMessageW(parent, iconMsg, 0, reinterpret_cast<LPARAM>(icons.get()))) icons.release();
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}

void FolderTree::addChildren(const TreeBatch& batch) {
    // Nodes in reading_ are alive (deleted ones leave in TVN_DELETEITEM); the read number spots a reused handle.
    auto it = reading_.find(batch.node);
    if (it == reading_.end() || it->second != batch.read) return;
    reading_.erase(it);
    if (!batch.node) {   // the user folders go after Bookmarks (always the first root) and before This PC
        HTREEITEM after = TreeView_GetRoot(tv_);
        for (const auto& c : batch.children) after = add(TVI_ROOT, c.path, c.text, c.icon, false, after);
        return;
    }
    if (batch.children.empty()) {
        // Keep the arrow when the read failed, and on Bookmarks (filled later, 5.6); others have no subfolders.
        if (batch.failed || batch.node == TreeView_GetRoot(tv_)) return;
        TVITEMW item{};
        item.mask = TVIF_CHILDREN;
        item.hItem = batch.node;
        item.cChildren = 0;
        TreeView_SetItem(tv_, &item);
        return;
    }
    for (const auto& c : batch.children) add(batch.node, c.path, c.text, c.icon >= 0 ? c.icon : folderIcon(), c.shortcut);
    TreeView_Expand(tv_, batch.node, TVE_EXPAND);
}

bool FolderTree::pathAt(POINT pt, std::wstring* path) const {
    HTREEITEM node = nullptr;
    if (pt.x == -1 && pt.y == -1) {
        node = TreeView_GetSelection(tv_);
    } else {
        TVHITTESTINFO hit{};
        hit.pt = pt;
        ScreenToClient(tv_, &hit.pt);
        node = TreeView_HitTest(tv_, &hit);
    }
    if (!node) return false;
    *path = pathOf(node);
    return true;
}

bool FolderTree::onNotify(NMHDR* nm, std::wstring* open) {
    switch (nm->code) {
    case TVN_ITEMEXPANDINGW: {
        auto* tv = reinterpret_cast<NMTREEVIEWW*>(nm);
        if (tv->action & TVE_EXPAND) startReading(tv->itemNew.hItem);
        return false;
    }
    case TVN_ITEMEXPANDEDW: {
        // Forget the subfolders of a collapsed node: the next expand reads them fresh.
        auto* tv = reinterpret_cast<NMTREEVIEWW*>(nm);
        if (tv->action & TVE_COLLAPSE) TreeView_Expand(tv_, tv->itemNew.hItem, TVE_COLLAPSE | TVE_COLLAPSERESET);
        return false;
    }
    case TVN_DELETEITEMW:
        // The handle dies with the node (collapse, fill(), shutdown); a read still in flight must not insert under it.
        reading_.erase(reinterpret_cast<NMTREEVIEWW*>(nm)->itemOld.hItem);
        paths_.erase(reinterpret_cast<NMTREEVIEWW*>(nm)->itemOld.hItem);
        return false;
    case TVN_SELCHANGEDW: {
        auto* tv = reinterpret_cast<NMTREEVIEWW*>(nm);
        if (tv->action == TVC_BYMOUSE || tv->action == TVC_BYKEYBOARD) {
            *open = pathOf(tv->itemNew.hItem);
            return true;
        }
        return false;
    }
    case NM_CLICK: {
        // Clicking the node that is already selected sends no change, so open it here.
        TVHITTESTINFO hit{};
        GetCursorPos(&hit.pt);
        ScreenToClient(tv_, &hit.pt);
        HTREEITEM node = TreeView_HitTest(tv_, &hit);
        if (node && node == TreeView_GetSelection(tv_) && (hit.flags & (TVHT_ONITEM | TVHT_ONITEMRIGHT))) {
            *open = pathOf(node);
            return true;
        }
        return false;
    }
    }
    return false;
}
