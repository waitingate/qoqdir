#pragma once
// The file list: a "virtual" ListView that stores no items itself. It asks for
// the text of each visible row, so a folder with 100,000 files shows instantly.
//
// items_ holds the folder, view_ holds the indices that pass the filter.
// Row i on screen is always items_[view_[i]]; update() is the only place that
// changes view_ (together with items_), and it always clears the ListView's
// selection with it, so no row or selection can point at the wrong file.

#include "folder.h"

#include <functional>

class FileList {
public:
    void create(HWND parent, int id);
    HWND hwnd() const { return lv_; }

    void setListing(Listing&& listing);   // new folder contents; keeps sort and filter
    void setFilter(const std::wstring& filter);
    void setSort(SortKey key, bool ascending);
    SortKey sortKey() const { return key_; }
    bool sortAscending() const { return asc_; }

    const std::wstring& dir() const { return dir_; }
    int count() const { return int(view_.size()); }
    int total() const { return int(items_.size()); }
    const Item& at(int row) const { return items_[view_[row]]; }
    std::wstring pathAt(int row) const { return joinPath(dir_, at(row).name); }

    std::vector<int> selection() const;
    std::vector<std::wstring> selectedPaths() const;
    std::vector<std::wstring> selectedNames() const;
    int focused() const;
    std::wstring focusedName() const;
    // Selects the named items (case-insensitive) and returns the row of 'focus', or -1.
    int select(const std::vector<std::wstring>& names, const std::wstring& focus);
    void selectAll();
    int topRow() const;
    void scrollToRow(int row);
    void ensureVisible(int row);
    void rename(int row);
    bool renaming() const;

    std::vector<int> columnWidths() const;
    void setColumnWidths(const std::vector<int>& widths);

    // Handles display-only notifications. Returns false for ones the app must handle.
    bool onNotify(NMHDR* nm, LRESULT* result);

private:
    void update(bool keepSelection, const std::function<void()>& change);
    void showSortArrow();

    HWND lv_ = nullptr;
    std::wstring dir_;
    std::vector<Item> items_;
    std::vector<int> view_;
    std::wstring filter_;
    SortKey key_ = SortKey::Name;
    bool asc_ = true;
};
