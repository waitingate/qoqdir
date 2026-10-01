#pragma once
// The folder tree on the left. Subfolders are read only when a node is expanded,
// on a worker thread, so a slow or disconnected network drive never freezes the window.

#include "folder.h"

#include <set>

// Subfolders of one node, read on a worker thread and posted to the tree's parent window.
struct TreeBatch {
    unsigned fill = 0;   // which fill() the node belongs to; batches for an older tree are dropped
    HTREEITEM node = nullptr;
    struct Child {
        std::wstring path, text;
        int icon = -1;   // -1: plain folder icon
        bool shortcut = false;
    };
    std::vector<Child> children;
};

class FolderTree {
public:
    // `batchMsg` is posted to `parent` with a TreeBatch* (to pass to addChildren) when subfolders are read.
    // `bookmarks` is shown at the top of the tree, open, with its shortcuts as well as its folders.
    void create(HWND parent, int id, UINT batchMsg, const std::wstring& bookmarks);
    HWND hwnd() const { return tv_; }
    void fill();   // rebuilds the roots (user folders and This PC)
    void addChildren(const TreeBatch& batch);

    // Path of the node under a screen point, or of the selected node when pt is (-1,-1).
    bool pathAt(POINT screenPt, std::wstring* path) const;
    // Returns true and sets 'open' when the user chose a folder (or a bookmark shortcut).
    bool onNotify(NMHDR* nm, std::wstring* open);

private:
    HTREEITEM add(HTREEITEM parent, const std::wstring& path, const std::wstring& text, int icon, bool leaf = false);
    void startReading(HTREEITEM node);
    const std::wstring& pathOf(HTREEITEM node) const;

    HWND tv_ = nullptr, parent_ = nullptr;
    std::wstring bookmarks_;
    UINT batchMsg_ = 0;
    unsigned fill_ = 0;
    std::vector<std::wstring> paths_;   // node lParam indexes this
    std::set<HTREEITEM> reading_;       // nodes whose subfolders are being read
};
