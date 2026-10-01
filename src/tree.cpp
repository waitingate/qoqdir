#include "tree.h"

#include "shell.h"

#include <uxtheme.h>

#include <algorithm>
#include <thread>

namespace {

// Reads the subfolders of `dir` ("" = the drives). Runs on a worker thread, so it may block.
std::vector<TreeBatch::Child> readChildren(const std::wstring& dir) {
    std::vector<TreeBatch::Child> children;
    if (dir.empty()) {
        DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (!(mask & (1u << i))) continue;
            std::wstring root = std::wstring(1, wchar_t(L'A' + i)) + L":\\";
            SHFILEINFOW sfi{};
            SHGetFileInfoW(root.c_str(), 0, &sfi, sizeof sfi, SHGFI_DISPLAYNAME | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
            children.push_back({root, sfi.szDisplayName[0] ? sfi.szDisplayName : root.substr(0, 2), sfi.iIcon});
        }
        return children;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(joinPath(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return children;
    do {
        const wchar_t* n = fd.cFileName;
        bool dots = n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0));
        if (!dots && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN))
            children.push_back({joinPath(dir, n), n, -1});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(children.begin(), children.end(),
              [](const auto& a, const auto& b) { return StrCmpLogicalW(a.text.c_str(), b.text.c_str()) < 0; });
    return children;
}

}  // namespace

void FolderTree::create(HWND parent, int id, UINT batchMsg, const std::wstring& bookmarks) {
    parent_ = parent;
    bookmarks_ = bookmarks;
    batchMsg_ = batchMsg;
    tv_ = CreateWindowExW(0, WC_TREEVIEWW, L"",
                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_SHOWSELALWAYS |
                              TVS_TRACKSELECT | TVS_FULLROWSELECT | TVS_NOHSCROLL,
                          0, 0, 0, 0, parent, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    TreeView_SetExtendedStyle(tv_, TVS_EX_DOUBLEBUFFER | TVS_EX_FADEINOUTEXPANDOS,
                              TVS_EX_DOUBLEBUFFER | TVS_EX_FADEINOUTEXPANDOS);
    SetWindowTheme(tv_, L"Explorer", nullptr);
    TreeView_SetImageList(tv_, systemImageList(), TVSIL_NORMAL);
}

void FolderTree::fill() {
    ++fill_;   // results still being read for the old nodes will be dropped
    reading_.clear();
    TreeView_DeleteAllItems(tv_);
    paths_.clear();
    add(TVI_ROOT, bookmarks_, L"Bookmarks", folderIcon());
    const KNOWNFOLDERID* folders[] = {&FOLDERID_Desktop, &FOLDERID_Downloads, &FOLDERID_Documents,
                                      &FOLDERID_Pictures, &FOLDERID_Music,    &FOLDERID_Videos};
    for (const KNOWNFOLDERID* id : folders) {
        std::wstring path = knownFolder(*id);
        SHFILEINFOW sfi{};
        if (path.empty() || !SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof sfi,
                                            SHGFI_DISPLAYNAME | SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
            continue;
        add(TVI_ROOT, path, sfi.szDisplayName, sfi.iIcon);
    }
    SHFILEINFOW pc{};
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &pidl))) {
        SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &pc, sizeof pc, SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        CoTaskMemFree(pidl);
    }
    HTREEITEM thisPc = add(TVI_ROOT, L"", L"This PC", pc.iIcon);
    TreeView_Expand(tv_, thisPc, TVE_EXPAND);
}

HTREEITEM FolderTree::add(HTREEITEM parent, const std::wstring& path, const std::wstring& text, int icon) {
    paths_.push_back(path);
    TVINSERTSTRUCTW ins{};
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_CHILDREN;
    ins.item.pszText = const_cast<wchar_t*>(text.c_str());
    ins.item.iImage = ins.item.iSelectedImage = icon;
    ins.item.lParam = LPARAM(paths_.size() - 1);
    ins.item.cChildren = 1;   // unknown until expanded
    return TreeView_InsertItem(tv_, &ins);
}

const std::wstring& FolderTree::pathOf(HTREEITEM node) const {
    TVITEMW item{};
    item.mask = TVIF_PARAM;
    item.hItem = node;
    TreeView_GetItem(tv_, &item);
    return paths_.at(size_t(item.lParam));
}

void FolderTree::startReading(HTREEITEM node) {
    if (TreeView_GetChild(tv_, node) || !reading_.insert(node).second) return;   // already read, or being read
    std::wstring dir = pathOf(node);
    HWND parent = parent_;
    UINT msg = batchMsg_;
    unsigned fill = fill_;
    std::thread([parent, msg, fill, node, dir] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        auto batch = std::make_unique<TreeBatch>();
        batch->fill = fill;
        batch->node = node;
        batch->children = readChildren(dir);
        if (PostMessageW(parent, msg, 0, reinterpret_cast<LPARAM>(batch.get()))) batch.release();
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}

void FolderTree::addChildren(const TreeBatch& batch) {
    // Nodes are only ever deleted by fill(), so a batch from the current fill has a valid node.
    if (batch.fill != fill_ || !reading_.erase(batch.node)) return;
    if (batch.children.empty()) {
        TVITEMW item{};
        item.mask = TVIF_CHILDREN;
        item.hItem = batch.node;
        item.cChildren = 0;
        TreeView_SetItem(tv_, &item);
        return;
    }
    for (const auto& c : batch.children) add(batch.node, c.path, c.text, c.icon >= 0 ? c.icon : folderIcon());
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
        if (node && node == TreeView_GetSelection(tv_) && (hit.flags & TVHT_ONITEM)) {
            *open = pathOf(node);
            return true;
        }
        return false;
    }
    }
    return false;
}
