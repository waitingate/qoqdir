#pragma once
// The file list: a virtual ListView that asks for each visible row's text, so 100,000 files show at once.
// Row i is items_[view_[i]] (view_: the items that pass the filter). Only update() changes them, and it
// clears the selection with them, so no row or selection can point at the wrong file.

#include "folder.h"
#include "shell.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>

class FileList {
public:
    // Own icons (a .exe's, a .lnk's) are read for the rows the list draws, a screenful at a time (4.7): a drawn row
    // that lacks one posts `askMsg` to `parent`, which calls readWantedIcons; the read posts `iconMsg` (an
    // IconBatch*), for setIcons.
    void create(HWND parent, int id, UINT askMsg, UINT iconMsg);
    HWND hwnd() const { return lv_; }

    void setListing(Listing&& listing);   // new folder contents; keeps sort and filter
    void clear();   // no folder at all: while a tab switch reads its folder, or the folder is not available
    void readWantedIcons();                  // on a worker: the own icons drawn rows asked for
    void setIcons(const IconBatch& batch);   // adds the own icons a read found
    void setFilter(std::wstring filter);
    void setShowHidden(bool show);   // hidden items are always read; they are listed only while this is on
    bool showHidden() const { return showHidden_; }
    void setSort(SortKey key, bool ascending);
    SortKey sortKey() const { return key_; }
    bool sortAscending() const { return asc_; }

    const std::wstring& dir() const { return dir_; }
    bool shows(const std::wstring& path) const { return shown_ && samePath(dir_, path); }   // "" is This PC
    int count() const { return int(view_.size()); }
    int total() const { return total_; }   // the items a filter would narrow down (not the hidden ones)
    const Item& at(int row) const { return items_[view_[row]]; }
    std::wstring pathAt(int row) const { return joinPath(dir_, at(row).name); }
    bool hasFolder(const std::wstring& name) const;   // a subfolder of that name, shown or filtered out
    std::vector<std::wstring> treeNames() const;   // subfolders and shortcuts (not hidden ones), sorted: what the tree may show

    std::vector<int> selection() const;
    std::vector<Item> selectedItems() const;   // with their attributes, size and date: see shell.h
    std::vector<std::wstring> selectedNames() const;
    int focused() const;
    std::wstring focusedName() const;
    // Selects the named items (case-insensitive) and returns the row of 'focus', or -1.
    int select(const std::vector<std::wstring>& names, const std::wstring& focus);
    int focusRow(int row);   // gives 'row' the keyboard focus (the selection stays); returns it, or -1 if none
    void selectAll();
    int topRow() const;
    void scrollToRow(int row);
    void ensureVisible(int row);
    void rename(int row);
    bool renaming() const;

    std::vector<int> columnWidths() const;
    void setColumnWidths(const std::vector<int>& widths);
    // The line under each row (5.2): drawn while the list paints, in this colour (CLR_NONE: none).
    void setLineColor(COLORREF color);
    COLORREF lineColor() const { return lineColor_; }
    // The own icons read so far for the folder shown (the self-test).
    int arrivalSorts() const { return arrivalSorts_; }   // listings it had to sort itself, as they came (the self-test)
    size_t ownIconCount() const { return ownIcons_.size(); }
    bool ownIconKnown(const std::wstring& name) const { return ownIcons_.count(name) > 0; }
    // The row at a screen point, or -1: none over the column header, which Windows' hit test gives to the first row or
    // to one scrolled behind it, and none below the last row.
    int rowAt(POINT screenPt) const;
    // During a drag (5.8): the folder a drop at that screen point goes into when it is over a folder's row, or a
    // shortcut's to a local folder; none elsewhere, nor when 'screenPt' is null (the drag left or ended). The row
    // lights up, unless the drop would leave the 'dragged' files where they are (4.8). In a folder on the network no
    // row takes a drop: each would be asked about on this thread as the pointer crosses it (4.1).
    std::optional<std::wstring> dropFolder(const POINT* screenPt, const std::vector<std::wstring>& dragged);

    // Handles display-only notifications. Returns false for ones the app must handle.
    bool onNotify(NMHDR* nm, LRESULT* result);

private:
    void update(bool keepSelection, const std::function<void()>& change);
    void showSortArrow();
    int iconOf(const Item& it) const;   // its own icon if read by now, else its type's
    void wantIcon(const std::wstring& name);   // a drawn row lacks its own icon
    int litRow() const { return dropLit_ ? overRow_ : -1; }   // the row a drag lights up, or -1

    HWND lv_ = nullptr;
    HWND parent_ = nullptr;
    UINT askMsg_ = 0, iconMsg_ = 0;
    std::wstring dir_;
    bool shown_ = false;   // dir_ is a folder that was read, not the "" of a cleared list
    int arrivalSorts_ = 0;
    std::vector<Item> items_;
    std::vector<int> view_;
    std::unordered_map<std::wstring, int> ownIcons_;   // file name -> own icon, for the folder shown
    std::unordered_set<std::wstring> asked_;           // names whose own icon was asked for since the listing came
    std::vector<std::wstring> wanted_;                 // ...of which these are still to read
    bool askPosted_ = false;                           // askMsg is on its way
    std::optional<std::wstring> readingFor_;           // the folder a read of own icons is out for
    std::wstring filter_;
    bool showHidden_ = false;
    int total_ = 0;
    SortKey key_ = SortKey::Name;
    bool asc_ = true;
    COLORREF lineColor_ = CLR_NONE;   // the line under each row (setLineColor)
    int overRow_ = -1;                         // the row a drag is over (dropFolder), or -1
    std::optional<std::wstring> dropFolder_;   // ...the folder it takes the drop into, if any
    bool dropLit_ = false;                     // ...and whether it lights up: not for one the files are in
};
