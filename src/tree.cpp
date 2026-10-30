// The folder tree (tree.h): reads on worker threads, their batches of children, and drops onto nodes.
#include "tree.h"

#include "shell.h"

#include <algorithm>
#include <unordered_map>

// ---------------------------------------------------------------- reading, on a worker thread

namespace {

// Subfolders of `dir` ("" = the drives), and its shortcuts if asked, into `batch`. Worker thread: may block.
void readChildren(TreeBatch& batch, const std::wstring& dir, bool shortcuts) {
    std::vector<TreeBatch::Child>& children = batch.children;
    if (dir.empty()) {   // the drives (not via listFolder: its free-space check can wait on a network drive)
        for (const std::wstring& root : driveRoots()) {
            SHFILEINFOW sfi = fileInfo(root, 0, SHGFI_DISPLAYNAME | SHGFI_SYSICONINDEX | SHGFI_SMALLICON).value_or(SHFILEINFOW{});
            children.push_back({root, sfi.szDisplayName[0] ? sfi.szDisplayName : root.substr(0, 2), sfi.iIcon});
        }
        return;
    }
    Listing listing = listFolder(dir, false);
    batch.failed = listing.error != 0;
    std::erase_if(listing.items, [&](const Item& it) { return !it.isDir() && !(shortcuts && isShortcut(it.name)); });
    sortItems(listing.items, SortKey::Name, true);   // folders first, in natural order
    for (const Item& it : listing.items) {
        std::wstring path = joinPath(dir, it.name);
        if (it.isDir()) {
            children.push_back({path, it.name, -1});
        } else {
            // From the .lnk alone: its name and a folder or file-type icon. Its own icon can wait on a server
            // that is off, so it follows later (startWorker).
            bool folder = false;
            std::optional<std::wstring> target = shortcutTarget(path, &folder);
            SHFILEINFOW name = fileInfo(path, 0, SHGFI_DISPLAYNAME).value_or(SHFILEINFOW{});
            std::wstring probe = folder ? L"x" : target.value_or(it.name);   // only the extension matters
            SHFILEINFOW icon = fileInfo(probe, folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
                                        SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON)
                                   .value_or(SHFILEINFOW{});
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
        if (path.empty()) continue;
        if (auto sfi = fileInfo(path, 0, SHGFI_DISPLAYNAME | SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
            batch.children.push_back({path, sfi->szDisplayName, sfi->iIcon});
    }
}

}  // namespace

// ---------------------------------------------------------------- the tree

void FolderTree::create(HWND parent, int id, UINT batchMsg, UINT iconMsg, UINT_PTR fillTimer,
                        const std::wstring& bookmarks) {
    parent_ = parent;
    bookmarks_ = bookmarks;
    batchMsg_ = batchMsg;
    iconMsg_ = iconMsg;
    fillTimer_ = fillTimer;
    // TVS_LINESATROOT gives the roots an expand arrow; without TVS_HASLINES no lines show, as in Explorer.
    tv_ = CreateWindowExW(0, WC_TREEVIEWW, L"",
                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS |
                              TVS_TRACKSELECT | TVS_FULLROWSELECT | TVS_NOHSCROLL,
                          0, 0, 0, 0, parent, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    TreeView_SetExtendedStyle(tv_, TVS_EX_DOUBLEBUFFER | TVS_EX_FADEINOUTEXPANDOS,
                              TVS_EX_DOUBLEBUFFER | TVS_EX_FADEINOUTEXPANDOS);
    setTheme(tv_, L"Explorer");
    TreeView_SetImageList(tv_, systemImageList(), TVSIL_NORMAL);
}

void FolderTree::fill() {
    TreeView_DeleteAllItems(tv_);   // forgets each node's path and its read in flight too (TVN_DELETEITEM)
    HTREEITEM bookmarksNode = add(TVI_ROOT, bookmarks_, L"Bookmarks", folderIcon());
    startWorker(nullptr, readUserFolders);   // the user folders arrive later, like the drives
    SHFILEINFOW pc{};
    if (Pidl pidl = pidlFromPath(L""))
        SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl.get()), 0, &pc, sizeof pc,
                       SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    HTREEITEM thisPc = add(TVI_ROOT, L"", L"This PC", pc.iIcon);
    TreeView_Expand(tv_, bookmarksNode, TVE_EXPAND);
    TreeView_Expand(tv_, thisPc, TVE_EXPAND);
}

HTREEITEM FolderTree::lastRoot() const {
    HTREEITEM last = TreeView_GetRoot(tv_);
    for (HTREEITEM n = last; n; n = TreeView_GetNextSibling(tv_, n)) last = n;
    return last;
}

// Drives came or went: re-read This PC only, so expanded folders stay open (4.5); its read in flight is dropped.
void FolderTree::refreshDrives() {
    HTREEITEM pc = lastRoot();
    if (!TreeView_GetChild(tv_, pc)) {
        // No drives shown yet: their first read is still in flight or waits to go in (read again, so the drive that
        // came or went is in the list), or This PC is closed and unread (its next expand reads them).
        if (reading_.count(pc) || filling(pc)) {
            forgetReads(pc);
            startReading(pc);
        }
        return;
    }
    forgetReads(pc);
    TreeView_Expand(tv_, pc, TVE_COLLAPSE | TVE_COLLAPSERESET);   // forget the drives; the next expand reads them
    TreeView_Expand(tv_, pc, TVE_EXPAND);   // and read them again
}

// Each folder whose subfolders are shown is read again, and each known to have none (it may have some now).
void FolderTree::refresh() {
    for (HTREEITEM n = TreeView_GetRoot(tv_); n; n = TreeView_GetNextVisible(tv_, n)) reread(n);
}

// The same for one folder's nodes: the folder shown in the list when its subfolders changed, or the parent of a node
// whose own menu was used (the node's folder may be gone).
void FolderTree::refreshFolder(const std::wstring& dir) {
    for (const auto& [node, info] : paths_)
        if (samePath(info.path, dir)) reread(node);
}

void FolderTree::reread(HTREEITEM n) {
    // A read already out may have listed the folder before the change this is for: one more follows it.
    if (reading_.count(n)) {
        stale_.insert(n);
        return;
    }
    TVITEMW item{};
    item.mask = TVIF_CHILDREN;
    item.hItem = n;
    TreeView_GetItem(tv_, &item);
    // An empty Bookmarks keeps its arrow (addChildren), so the arrow can't tell: its last read does.
    bool emptyRoot = bookmarksEmpty_ && n == TreeView_GetRoot(tv_);
    // Children still waiting to go in were read before the change too.
    if (TreeView_GetChild(tv_, n) || filling(n) || (!item.cChildren && !shortcutNode(n)) || emptyRoot) startReading(n);
}

HTREEITEM FolderTree::add(HTREEITEM parent, const std::wstring& path, const std::wstring& text, int icon,
                          bool shortcut, HTREEITEM after) {
    TVINSERTSTRUCTW ins{};
    ins.hParent = parent;
    ins.hInsertAfter = after;
    ins.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
    ins.item.pszText = const_cast<wchar_t*>(text.c_str());
    ins.item.iImage = ins.item.iSelectedImage = icon;
    ins.item.cChildren = shortcut ? 0 : 1;   // a shortcut has no arrow; a folder's children are unknown until expanded
    HTREEITEM node = TreeView_InsertItem(tv_, &ins);
    if (node) paths_[node] = {path, shortcut};
    return node;
}

const std::wstring& FolderTree::pathOf(HTREEITEM node) const {
    return paths_.at(node).path;   // a node that is not there is a dead handle: stop loudly (4.10)
}

bool FolderTree::shortcutNode(HTREEITEM node) const { return paths_.at(node).shortcut; }

void FolderTree::startReading(HTREEITEM node) {
    if (reading_.count(node)) return;   // being read
    std::wstring dir = pathOf(node);
    HTREEITEM top = node;   // shortcuts show under Bookmarks, the first root, at any depth (5.6)
    while (HTREEITEM up = TreeView_GetParent(tv_, top)) top = up;
    bool shortcuts = top == TreeView_GetRoot(tv_);
    startWorker(node, [dir, shortcuts](TreeBatch& batch) { readChildren(batch, dir, shortcuts); });
}

void FolderTree::startWorker(HTREEITEM node, std::function<void(TreeBatch&)> reader) {
    unsigned read = reading_[node] = ++reads_;
    inBackground([parent = parent_, msg = batchMsg_, iconMsg = iconMsg_, read, node, reader = std::move(reader)] {
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
        post(parent, msg, 0, std::move(batch));
        // Then each shortcut's own icon, which asks the target (maybe a server that is off), as the list does.
        if (!shortcuts.empty())
            post(parent, iconMsg, reinterpret_cast<WPARAM>(node), std::make_unique<IconBatch>(readOwnIcons(dir, shortcuts)));
    });
}

void FolderTree::addChildren(TreeBatch& batch) {
    // Nodes in reading_ are alive (deleted ones leave in TVN_DELETEITEM); the read number spots a reused handle.
    auto it = reading_.find(batch.node);
    if (it == reading_.end() || it->second != batch.read) return;
    reading_.erase(it);
    dropFill(batch.node);   // what an older read still had to put in: this one is newer
    // A change while this read was out (reread) may be missing from it: one more read once it is shown.
    bool changed = stale_.erase(batch.node);
    showChildren(batch);
    if (changed) reread(batch.node);
}

void FolderTree::showChildren(TreeBatch& batch) {
    if (!batch.node) {   // the user folders go after Bookmarks (always the first root) and before This PC
        HTREEITEM after = TreeView_GetRoot(tv_);
        for (const auto& c : batch.children) after = add(TVI_ROOT, c.path, c.text, c.icon, false, after);
        return;
    }
    if (batch.failed) return;   // keep what is shown, and the arrow: expanding again tries again
    if (batch.node == TreeView_GetRoot(tv_)) bookmarksEmpty_ = batch.children.empty();   // reread() reads it again
    // Read again (refresh): a folder whose subfolders are the same stays as it is. Otherwise they are put in afresh,
    // and what was open below closes (an open list changed item by item is slow: Windows repaints it for each).
    if (HTREEITEM c = TreeView_GetChild(tv_, batch.node)) {
        auto next = batch.children.begin();
        for (; c && next != batch.children.end() && pathOf(c) == next->path; c = TreeView_GetNextSibling(tv_, c)) ++next;
        if (!c && next == batch.children.end()) return;
        TreeView_Expand(tv_, batch.node, TVE_COLLAPSE | TVE_COLLAPSERESET);
    }
    // The arrow goes with the last subfolder, but Bookmarks keeps it (filled later, 5.6). It comes back with the first
    // new one: Windows won't open a node whose arrow is gone.
    bool any = !batch.children.empty();
    if (any || batch.node != TreeView_GetRoot(tv_)) {
        TVITEMW item{};
        item.mask = TVIF_CHILDREN;
        item.hItem = batch.node;
        item.cChildren = any ? 1 : 0;
        TreeView_SetItem(tv_, &item);
    }
    if (!any) return;
    fills_.push_back({batch.node, std::move(batch.children)});
    fillMore();
}

// A part at a time, so the window answers in between (4.7): each insert costs more where accessibility hooks are
// loaded, as an antivirus's are (120 us here), and 10,000 at once kept the window from answering for over a second.
// While input, painting or a timer waits, the rest waits for a timer, which Windows sends only after those (a posted
// message would come before them). Otherwise it goes on at once, by posting the timer's message: a timer comes 10 to
// 30 ms late, and those gaps made 10,000 take 2.3 s, not 1.4. Another thread's call needs no timer: Windows answers
// those before it hands over any posted message.
void FolderTree::fillMore() {
    KillTimer(parent_, fillTimer_);
    ULONGLONG start = GetTickCount64();
    while (!fills_.empty()) {
        if (GetTickCount64() - start >= 30) {
            if (HIWORD(GetQueueStatus(QS_INPUT | QS_PAINT | QS_TIMER)))
                SetTimer(parent_, fillTimer_, USER_TIMER_MINIMUM, nullptr);
            else
                PostMessageW(parent_, WM_TIMER, fillTimer_, 0);
            return;
        }
        Fill& f = fills_.front();
        if (f.children.empty()) {
            HTREEITEM node = f.node;
            fills_.erase(fills_.begin());
            TreeView_Expand(tv_, node, TVE_EXPAND);
            continue;
        }
        // Each at the front, the last one first: the same order, but Windows never walks the list to its end for each
        // one, which froze the window for seconds on a folder with 20,000 subfolders.
        TreeBatch::Child c = std::move(f.children.back());
        f.children.pop_back();
        add(f.node, c.path, c.text, c.icon >= 0 ? c.icon : folderIcon(), c.shortcut, TVI_FIRST);
    }
}

void FolderTree::dropFill(HTREEITEM node) {
    std::erase_if(fills_, [&](const Fill& f) { return f.node == node; });
}

bool FolderTree::filling(HTREEITEM node) const {
    return std::any_of(fills_.begin(), fills_.end(), [&](const Fill& f) { return f.node == node; });
}

void FolderTree::forgetReads(HTREEITEM node) {
    reading_.erase(node);
    stale_.erase(node);
    dropFill(node);
}

// The shortcuts' own icons, read after their node was shown (startWorker). The node may be gone by now.
void FolderTree::setIcons(HTREEITEM node, const IconBatch& batch) {
    if (!paths_.count(node)) return;
    std::unordered_map<std::wstring, int> icons;   // by lowercase path: one look-up per node, not one per pair
    for (const auto& [name, icon] : batch.icons) icons[toLower(joinPath(batch.dir, name))] = icon;
    for (HTREEITEM c = TreeView_GetChild(tv_, node); c; c = TreeView_GetNextSibling(tv_, c)) {
        auto found = icons.find(toLower(pathOf(c)));
        if (found == icons.end()) continue;
        TVITEMW item{};
        item.mask = TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        item.hItem = c;
        item.iImage = item.iSelectedImage = found->second;
        TreeView_SetItem(tv_, &item);
    }
    // Those still waiting to go in (behind another folder's, fillMore) take theirs in.
    for (Fill& f : fills_)
        if (f.node == node)
            for (TreeBatch::Child& c : f.children)
                if (auto found = icons.find(toLower(c.path)); found != icons.end()) c.icon = found->second;
}

// ---------------------------------------------------------------- the mouse: paths and drops

HTREEITEM FolderTree::nodeAt(POINT screenPt, UINT* where) const {
    TVHITTESTINFO hit{};
    hit.pt = screenPt;
    ScreenToClient(tv_, &hit.pt);
    HTREEITEM node = TreeView_HitTest(tv_, &hit);
    if (where) *where = hit.flags;
    return node;
}

bool FolderTree::pathAt(POINT pt, std::wstring* path, bool* shortcut) const {
    HTREEITEM node = pt.x == -1 && pt.y == -1 ? TreeView_GetSelection(tv_) : nodeAt(pt);
    if (!node) return false;
    *path = pathOf(node);
    if (shortcut) *shortcut = shortcutNode(node);
    return true;
}

// The node under the cursor lights up, and a closed one opens after 0.7 s (Windows asks again many times a second
// during a drag), so the folders inside it can be reached. Where a drop there goes is worked out once, as it lights up.
std::optional<std::wstring> FolderTree::dropTarget(const POINT* screenPt) {
    HTREEITEM node = screenPt ? nodeAt(*screenPt) : nullptr;
    if (node != TreeView_GetDropHilight(tv_)) {
        TreeView_SelectDropTarget(tv_, node);
        openAt_ = GetTickCount64() + 700;
        dropFolder_ = node ? localFolder(pathOf(node), shortcutNode(node)) : std::nullopt;
    } else if (node && openAt_ && GetTickCount64() >= openAt_) {
        TreeView_Expand(tv_, node, TVE_EXPAND);
        openAt_ = 0;   // once: a folder that can't be read would be read again and again
    }
    // With the lit node gone (a drive came or went, Bookmarks changed, the tree rebuilt under the pointer), the
    // cursor is now over empty space: the drop goes nowhere, not to the node that was lit here before.
    return node ? dropFolder_ : std::nullopt;
}

// ---------------------------------------------------------------- notifications

bool FolderTree::onNotify(NMHDR* nm, std::wstring* open, bool* shortcut) {
    auto chose = [&](HTREEITEM node) {   // report the node and whether it is a shortcut
        *open = pathOf(node);
        *shortcut = shortcutNode(node);
        return true;
    };
    switch (nm->code) {
    case TVN_ITEMEXPANDINGW: {
        auto* tv = reinterpret_cast<NMTREEVIEWW*>(nm);
        // Not a node whose subfolders are shown: addChildren expands the node it has just filled.
        if ((tv->action & TVE_EXPAND) && !TreeView_GetChild(tv_, tv->itemNew.hItem)) startReading(tv->itemNew.hItem);
        return false;
    }
    case TVN_ITEMEXPANDEDW: {
        // Forget the subfolders of a collapsed node: the next expand reads them fresh. A read of them still in
        // flight (F5, a drag held over it) is dropped too: its result would open the node again (4.5).
        auto* tv = reinterpret_cast<NMTREEVIEWW*>(nm);
        if (tv->action & TVE_COLLAPSE) {
            forgetReads(tv->itemNew.hItem);
            TreeView_Expand(tv_, tv->itemNew.hItem, TVE_COLLAPSE | TVE_COLLAPSERESET);
        }
        return false;
    }
    case TVN_DELETEITEMW: {
        // The handle dies with the node (collapse, fill(), shutdown); a read still in flight must not insert under it.
        HTREEITEM gone = reinterpret_cast<NMTREEVIEWW*>(nm)->itemOld.hItem;
        forgetReads(gone);
        paths_.erase(gone);
        return false;
    }
    case TVN_SELCHANGEDW: {
        auto* tv = reinterpret_cast<NMTREEVIEWW*>(nm);
        if (tv->action != TVC_BYMOUSE && tv->action != TVC_BYKEYBOARD) return false;
        HTREEITEM node = tv->itemNew.hItem;
        if (tv->action == TVC_BYKEYBOARD && shortcutNode(node)) {
            // Moving the selection with the keyboard opens a folder shortcut, but not a program or a web link: those
            // need Enter, a click or the menu, so arrowing through Bookmarks starts nothing (4.4, 5.6).
            bool folder = false;
            if (!(shortcutTarget(pathOf(node), &folder) && folder)) return false;
        }
        return chose(node);
    }
    case NM_CLICK: {
        // Clicking the node that is already selected sends no change, so open it here.
        POINT pt{};
        GetCursorPos(&pt);
        UINT where = 0;
        HTREEITEM node = nodeAt(pt, &where);
        if (node && node == TreeView_GetSelection(tv_) && (where & (TVHT_ONITEM | TVHT_ONITEMRIGHT)))
            return chose(node);
        return false;
    }
    }
    return false;
}
