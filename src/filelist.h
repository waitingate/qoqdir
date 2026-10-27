#pragma once
// The file list: a virtual ListView that asks for each visible row's text, so 100,000 files show at once.
// Row i is items_[view_[i]] (view_: the items that pass the filter). Only update() changes them, and it
// clears the selection with them, so no row or selection can point at the wrong file.

#include "folder.h"
#include "shell.h"

#include <functional>
#include <unordered_map>

class FileList {
public:
    void create(HWND parent, int id);
    HWND hwnd() const { return lv_; }

    void setListing(Listing&& listing);   // new folder contents; keeps sort and filter
    void clear();   // no folder at all: while a tab switch reads its folder, or the folder is not available
    void setIcons(const IconBatch& batch);   // the folder's own icons (a .exe, a .lnk), read after its listing
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

    // Handles display-only notifications. Returns false for ones the app must handle.
    bool onNotify(NMHDR* nm, LRESULT* result);

private:
    void update(bool keepSelection, const std::function<void()>& change);
    void showSortArrow();
    int iconOf(const Item& it) const;   // its own icon if read by now, else its type's

    HWND lv_ = nullptr;
    std::wstring dir_;
    bool shown_ = false;   // dir_ is a folder that was read, not the "" of a cleared list
    std::vector<Item> items_;
    std::vector<int> view_;
    std::unordered_map<std::wstring, int> ownIcons_;   // full path -> own icon, for the folder shown
    std::wstring filter_;
    bool showHidden_ = false;
    int total_ = 0;
    SortKey key_ = SortKey::Name;
    bool asc_ = true;
    COLORREF lineColor_ = CLR_NONE;   // the line under each row (setLineColor)
};
