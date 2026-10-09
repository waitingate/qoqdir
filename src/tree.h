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
    // Posts `batchMsg` (a TreeBatch*, for addChildren) and `iconMsg` (an IconBatch*, for setIcons) to `parent`.
    // `bookmarks` is the first root, open, with its shortcuts as well as its folders.
    void create(HWND parent, int id, UINT batchMsg, UINT iconMsg, const std::wstring& bookmarks);
    HWND hwnd() const { return tv_; }
    const std::wstring& bookmarks() const { return bookmarks_; }
    void fill();   // rebuilds the roots (user folders and This PC)
    void refreshDrives();   // reads This PC's drives again; the rest of the tree stays as it is
    void refresh();         // F5: reads again the folders whose subfolders are shown or known to be none (5.5)
    void refreshFolder(const std::wstring& dir);   // the same for that folder's nodes alone
    void addChildren(const TreeBatch& batch);
    void setIcons(const IconBatch& batch);

    // Path of the node under a screen point, or of the selected node when pt is (-1,-1).
    bool pathAt(POINT screenPt, std::wstring* path) const;
    // During a drag: the local folder under the cursor, or a bookmark shortcut's, that may take the drop (nullptr:
    // the drag left or ended).
    std::optional<std::wstring> dropTarget(const POINT* screenPt);
    // Returns true and sets 'open' when the user chose a folder (or a bookmark shortcut).
    bool onNotify(NMHDR* nm, std::wstring* open);

private:
    HTREEITEM add(HTREEITEM parent, const std::wstring& path, const std::wstring& text, int icon, bool leaf = false,
                  HTREEITEM after = TVI_LAST);
    void startReading(HTREEITEM node, bool again = false);   // again: also when its subfolders are shown
    void reread(HTREEITEM node);   // what refresh() does for one node
    // Runs `reader` on a worker thread to fill a batch, then posts it as the children of `node`.
    void startWorker(HTREEITEM node, std::function<void(TreeBatch&)> reader);
    const std::wstring& pathOf(HTREEITEM node) const;

    HWND tv_ = nullptr, parent_ = nullptr;
    std::wstring bookmarks_;
    UINT batchMsg_ = 0, iconMsg_ = 0;
    unsigned reads_ = 0;                      // numbers every read started
    std::map<HTREEITEM, std::wstring> paths_;   // each node's folder (or shortcut); a node leaves it when deleted
    std::map<HTREEITEM, unsigned> reading_;   // nodes whose subfolders are being read, by read number
    ULONGLONG openAt_ = 0;                    // when the node a drag lights up opens (0: done)
    std::optional<std::wstring> dropFolder_;  // and where a drop on that node goes
};
