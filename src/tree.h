#pragma once
// The folder tree. Subfolders are read on a worker thread when a node expands: a dead drive never freezes it.

#include "shell.h"

#include <functional>
#include <map>

// Subfolders of one node, read on a worker thread and posted to the tree's parent window.
struct TreeBatch {
    unsigned read = 0;   // the startReading() this answers; a node deleted and re-read since then has a newer one
    HTREEITEM node = nullptr;   // nullptr: the user folders, which are roots between Bookmarks and This PC
    bool failed = false;        // the folder could not be read, so nothing is known about its children
    struct Child {
        std::wstring path, text;
        int icon = -1;   // -1: plain folder icon
        bool shortcut = false;
    };
    std::vector<Child> children;
};

class FolderTree {
public:
    // Posts `batchMsg` (a TreeBatch*, for addChildren) and `iconMsg` (an IconBatch*, with its node as WPARAM, for
    // setIcons) to `parent`, and sets `fillTimer` on it (for fillMore). `bookmarks` is the first root, open; it and the
    // folders in it show shortcuts too.
    void create(HWND parent, int id, UINT batchMsg, UINT iconMsg, UINT_PTR fillTimer, const std::wstring& bookmarks);
    HWND hwnd() const { return tv_; }
    const std::wstring& bookmarks() const { return bookmarks_; }
    HTREEITEM lastRoot() const;   // This PC, the last root
    void fill();   // rebuilds the roots (user folders and This PC)
    void refreshDrives();   // reads This PC's drives again; the rest of the tree stays as it is
    void refresh();         // F5: reads again the folders whose subfolders are shown or known to be none (5.5)
    void refreshFolder(const std::wstring& dir);   // the same for that folder's nodes alone
    bool hasNode(const std::wstring& dir) const;   // the folder is in the tree (shown, or read to be)
    void addChildren(TreeBatch& batch);   // takes its children
    // More of a big folder's subfolders, as many as about 30 ms allows; the timer brings the rest (showChildren).
    void fillMore();
    UINT batchMessage() const { return batchMsg_; }   // what a read's result comes back as (self-test)
    bool readingBookmarks() const { return nodes_.at(TreeView_GetRoot(tv_)).read != 0; }   // a read is out (self-test)
    void setIcons(HTREEITEM node, const IconBatch& batch);

    // Path of the node under a screen point, or of the selected node when pt is (-1,-1). 'shortcut', when given,
    // receives whether that node is a bookmark shortcut (not a folder of that name, 5.6).
    bool pathAt(POINT screenPt, std::wstring* path, bool* shortcut = nullptr) const;
    // During a drag: the local folder under the cursor, or a bookmark shortcut's, that may take the drop (nullptr:
    // the drag left or ended).
    std::optional<std::wstring> dropTarget(const POINT* screenPt);
    // Returns true and sets 'open' when the user chose a folder (or a bookmark shortcut).
    bool onNotify(NMHDR* nm, std::wstring* open, bool* shortcut);

private:
    HTREEITEM add(HTREEITEM parent, const std::wstring& path, const std::wstring& text, int icon, bool shortcut = false,
                  HTREEITEM after = TVI_LAST);   // a bookmark shortcut gets no expand arrow
    void startReading(HTREEITEM node);
    void reread(HTREEITEM node);   // what refresh() does for one node
    void showChildren(TreeBatch& batch);   // addChildren's batch, put in the tree
    void dropFill(HTREEITEM node);         // its children still to go in won't (the node closed, went, or is read anew)
    bool filling(HTREEITEM node) const;    // its children are read but still to go in (they wait behind others')
    void forgetReads(HTREEITEM node);      // and its reads out, whose results must not land on it either
    // Runs `reader` on a worker thread to fill a batch, then posts it as the children of `node`.
    void startWorker(HTREEITEM node, std::function<void(TreeBatch&)> reader);
    const std::wstring& pathOf(HTREEITEM node) const;
    bool shortcutNode(HTREEITEM node) const;   // a bookmark shortcut, not a folder (so a folder named "x.lnk" is not)
    HTREEITEM nodeAt(POINT screenPt, UINT* where = nullptr) const;   // where: on which part of it (TVHT_*)

    HWND tv_ = nullptr, parent_ = nullptr;
    std::wstring bookmarks_;
    UINT batchMsg_ = 0, iconMsg_ = 0;
    UINT_PTR fillTimer_ = 0;
    struct Fill { HTREEITEM node; std::vector<TreeBatch::Child> children; };   // still to go in, the last first
    std::vector<Fill> fills_;
    unsigned reads_ = 0;                      // numbers every read started
    unsigned userFoldersRead_ = 0;            // the read of the user folders that is wanted
    struct Node {
        std::wstring path;                    // its folder, or its shortcut
        bool shortcut = false;                // a bookmark shortcut, not a folder
        unsigned read = 0;                    // the read of its subfolders that is out, by number (0: none)
        bool stale = false;                   // ...and it changed meanwhile: read once more after (5.5)
        bool none = false;                    // its last read found no subfolders
    };
    std::map<HTREEITEM, Node> nodes_;         // a node leaves when deleted, and what is known of its reads with it
    ULONGLONG openAt_ = 0;                    // when the node a drag lights up opens (0: done)
    std::optional<std::wstring> dropFolder_;  // and where a drop on that node goes
};
