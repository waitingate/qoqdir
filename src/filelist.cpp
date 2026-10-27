// The file list (filelist.h): the virtual ListView's rows, sorting, filtering and selection.
#include "filelist.h"

#include "shell.h"

#include <algorithm>
#include <unordered_set>

namespace {
const wchar_t* const kColumns[] = {L"Name", L"Date modified", L"Type", L"Size"};
constexpr int kColumnCount = 4;
}  // namespace

// ---------------------------------------------------------------- contents

void FileList::create(HWND parent, int id) {
    lv_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS |
                              LVS_EDITLABELS | LVS_SHAREIMAGELISTS,
                          0, 0, 0, 0, parent, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(lv_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    setTheme(lv_, L"Explorer");
    // The column header in Windows' plain flat look: a light gray that goes with the list's (the themed one is white).
    HWND header = ListView_GetHeader(lv_);
    setTheme(header, L"");
    SetWindowLongPtrW(header, GWL_STYLE, GetWindowLongPtrW(header, GWL_STYLE) | HDS_FLAT);
    ListView_SetImageList(lv_, systemImageList(), LVSIL_SMALL);
    for (int i = 0; i < kColumnCount; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt = i == 3 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        col.cx = 100;
        col.pszText = const_cast<wchar_t*>(kColumns[i]);
        ListView_InsertColumn(lv_, i, &col);
    }
    showSortArrow();
}

void FileList::setListing(Listing&& listing) {
    update(false, [&] {
        dir_ = std::move(listing.path);
        shown_ = true;
        items_ = std::move(listing.items);
        for (Item& it : items_)   // cached per extension, so this is quick; lets Type sort by what is shown
            if (it.type.empty()) it.type = typeName(it);
        sortItems(items_, key_, asc_);
    });
}

void FileList::clear() {
    setListing(Listing{});
    shown_ = false;
}

// A late batch for a folder no longer shown is of no use. A batch replaces the last one: the map never outgrows a
// folder, and a program replaced by another gets its new icon on the next read.
void FileList::setIcons(const IconBatch& batch) {
    if (!shows(batch.dir)) return;
    ownIcons_.clear();
    for (const auto& [name, icon] : batch.icons) ownIcons_[joinPath(batch.dir, name)] = icon;
    InvalidateRect(lv_, nullptr, FALSE);
}

void FileList::setFilter(std::wstring filter) {
    if (filter == filter_) return;
    update(true, [&] { filter_ = filter; });
}

void FileList::setShowHidden(bool show) {
    update(true, [&] { showHidden_ = show; });
}

void FileList::setSort(SortKey key, bool ascending) {
    update(true, [&] {
        key_ = key;
        asc_ = ascending;
        sortItems(items_, key_, asc_);
    });
    showSortArrow();
}

void FileList::update(bool keepSelection, const std::function<void()>& change) {
    std::vector<std::wstring> names;
    std::wstring focus;
    if (keepSelection) {
        names = selectedNames();
        focus = focusedName();
    }
    // The edit box knows a row, not a file: close it before rows move, or Enter renames whatever lands there.
    if (renaming()) ListView_CancelEditLabel(lv_);
    // Rebuild view_ before any ListView call: those can ask for a row's text at once.
    change();
    view_.clear();
    total_ = 0;
    for (int i = 0; i < int(items_.size()); ++i) {
        if (!showHidden_ && (items_[i].attrs & FILE_ATTRIBUTE_HIDDEN)) continue;
        ++total_;
        if (matchesFilter(items_[i], filter_)) view_.push_back(i);
    }
    ListView_SetItemState(lv_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemCountEx(lv_, int(view_.size()), LVSICF_NOSCROLL);
    InvalidateRect(lv_, nullptr, FALSE);
    select(names, focus);
    // select() left the focus on a row; the old scroll position may be past the end.
    ensureVisible(focused());
}

// ---------------------------------------------------------------- what is shown

bool FileList::hasFolder(const std::wstring& name) const {
    return std::any_of(items_.begin(), items_.end(), [&](const Item& it) { return it.isDir() && samePath(it.name, name); });
}

std::vector<std::wstring> FileList::treeNames() const {
    std::vector<std::wstring> names;
    for (const Item& it : items_)
        if ((it.isDir() || isShortcut(it.name)) && !(it.attrs & FILE_ATTRIBUTE_HIDDEN)) names.push_back(it.name);
    std::sort(names.begin(), names.end());
    return names;
}

int FileList::iconOf(const Item& it) const {
    if (it.icon >= 0) return it.icon;
    if (hasOwnIcon(it)) {
        auto found = ownIcons_.find(joinPath(dir_, it.name));
        if (found != ownIcons_.end()) return found->second;
    }
    return typeIcon(it);
}

std::vector<int> FileList::selection() const {
    std::vector<int> rows;
    for (int i = ListView_GetNextItem(lv_, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(lv_, i, LVNI_SELECTED))
        if (i < count()) rows.push_back(i);
    return rows;
}

std::vector<Item> FileList::selectedItems() const {
    std::vector<Item> items;
    for (int row : selection()) items.push_back(at(row));
    return items;
}

std::vector<std::wstring> FileList::selectedNames() const {
    std::vector<std::wstring> names;
    for (int row : selection()) names.push_back(at(row).name);
    return names;
}

int FileList::focused() const {
    int row = ListView_GetNextItem(lv_, -1, LVNI_FOCUSED);
    return row < count() ? row : -1;
}

std::wstring FileList::focusedName() const {
    int row = focused();
    return row >= 0 ? at(row).name : L"";
}

// ---------------------------------------------------------------- selection and scrolling

int FileList::select(const std::vector<std::wstring>& names, const std::wstring& focus) {
    ListView_SetItemState(lv_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    int found = -1;
    if (!names.empty() || !focus.empty()) {
        // A wanted name takes its own exact-case row when the listing has one, so in a case-sensitive folder (WSL
        // makes them) "readme" and "README" each take only their own, not both. A name no row matches exactly falls
        // back to case-insensitive, needed for the folder you came from when its letter case differs (go up).
        std::unordered_set<std::wstring> exact, loose;
        for (const std::wstring& n : names) {
            bool present = false;
            for (int row = 0; row < count() && !present; ++row) present = at(row).name == n;
            if (present) exact.insert(n);
            else loose.insert(toLower(n));
        }
        std::wstring focusLower = toLower(focus);
        std::vector<int> rows;
        for (int row = 0; row < count(); ++row) {
            const std::wstring& name = at(row).name;
            if (exact.count(name) || loose.count(toLower(name))) rows.push_back(row);
            if (found < 0 && at(row).name == focus) found = row;          // exact focus first
        }
        if (found < 0 && !focus.empty())                                  // then case-insensitive
            for (int row = 0; row < count() && found < 0; ++row)
                if (toLower(at(row).name) == focusLower) found = row;
        // All of them (Ctrl+A, then a sort or a filter key) in one message, not one per row and its notification.
        if (!rows.empty() && int(rows.size()) == count()) selectAll();
        else for (int row : rows) ListView_SetItemState(lv_, row, LVIS_SELECTED, LVIS_SELECTED);
    }
    focusRow(found >= 0 ? found : 0);   // something always has keyboard focus, so arrow keys work right away
    return found;
}

int FileList::focusRow(int row) {
    if (row < 0 || row >= count()) return -1;
    ListView_SetItemState(lv_, -1, 0, LVIS_FOCUSED);
    ListView_SetItemState(lv_, row, LVIS_FOCUSED, LVIS_FOCUSED);
    ListView_SetSelectionMark(lv_, row);
    return row;
}

void FileList::selectAll() {
    ListView_SetItemState(lv_, -1, LVIS_SELECTED, LVIS_SELECTED);
}

int FileList::topRow() const { return ListView_GetTopIndex(lv_); }

void FileList::scrollToRow(int row) {
    if (row < 0 || count() == 0) return;
    RECT r{};
    if (!ListView_GetItemRect(lv_, 0, &r, LVIR_BOUNDS)) return;
    ListView_Scroll(lv_, 0, (row - topRow()) * (r.bottom - r.top));
}

void FileList::ensureVisible(int row) {
    if (row >= 0 && row < count()) ListView_EnsureVisible(lv_, row, FALSE);
}

// ---------------------------------------------------------------- renaming and columns

void FileList::rename(int row) {
    if (row < 0 || row >= count() || dir_.empty()) return;
    SetFocus(lv_);
    ListView_EditLabel(lv_, row);
}

bool FileList::renaming() const { return ListView_GetEditControl(lv_) != nullptr; }

std::vector<int> FileList::columnWidths() const {
    std::vector<int> w;
    for (int i = 0; i < kColumnCount; ++i) w.push_back(ListView_GetColumnWidth(lv_, i));
    return w;
}

void FileList::setColumnWidths(const std::vector<int>& widths) {
    for (int i = 0; i < kColumnCount && i < int(widths.size()); ++i) ListView_SetColumnWidth(lv_, i, widths[i]);
}

void FileList::showSortArrow() {
    HWND header = ListView_GetHeader(lv_);
    for (int i = 0; i < kColumnCount; ++i) {
        HDITEMW hd{};
        hd.mask = HDI_FORMAT;
        Header_GetItem(header, i, &hd);
        hd.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == int(key_)) hd.fmt |= asc_ ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(header, i, &hd);
    }
}

void FileList::setLineColor(COLORREF color) {
    lineColor_ = color;
    InvalidateRect(lv_, nullptr, FALSE);
}

// ---------------------------------------------------------------- notifications

bool FileList::onNotify(NMHDR* nm, LRESULT* result) {
    switch (nm->code) {
    case LVN_GETDISPINFOW: {
        LVITEMW& lvi = reinterpret_cast<NMLVDISPINFOW*>(nm)->item;
        if (lvi.iItem < 0 || lvi.iItem >= count()) return true;
        const Item& it = at(lvi.iItem);
        if ((lvi.mask & LVIF_TEXT) && lvi.pszText && lvi.cchTextMax > 0) {
            std::wstring text;
            switch (lvi.iSubItem) {
            case 0: text = it.shown(); break;
            case 1: text = formatTime(it.modified); break;
            case 2: text = typeName(it); break;
            case 3: if (!it.isDir()) text = formatSize(it.size); break;
            }
            lstrcpynW(lvi.pszText, text.c_str(), lvi.cchTextMax);
        }
        if (lvi.mask & LVIF_IMAGE) lvi.iImage = iconOf(it);   // not stored in the item: a file's own icon may come later
        return true;
    }
    case LVN_COLUMNCLICK: {
        auto key = SortKey(reinterpret_cast<NMLISTVIEW*>(nm)->iSubItem);
        // Names and types read best A-Z; dates and sizes newest/largest first.
        bool asc = key == key_ ? !asc_ : (key == SortKey::Name || key == SortKey::Type);
        setSort(key, asc);
        return true;
    }
    case LVN_BEGINLABELEDITW: {
        int row = reinterpret_cast<NMLVDISPINFOW*>(nm)->item.iItem;
        if (dir_.empty() || row < 0 || row >= count()) {
            *result = TRUE;   // drives cannot be renamed here
            return true;
        }
        // Select the name without its extension, like Explorer.
        const Item& it = at(row);
        size_t dot = it.isDir() ? std::wstring::npos : it.name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0)
            PostMessageW(ListView_GetEditControl(lv_), EM_SETSEL, 0, LPARAM(dot));
        *result = FALSE;
        return true;
    }
    case NM_CUSTOMDRAW: {
        // A pale line under each row, so the eye can follow it across the columns (5.2), drawn once the row is: not
        // under a selected, focused or hot row, whose own frame marks it. Rows that aren't there get none.
        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(nm);
        *result = CDRF_DODEFAULT;
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) *result = CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) *result = CDRF_NOTIFYPOSTPAINT;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT && lineColor_ != CLR_NONE) {
            int row = int(cd->nmcd.dwItemSpec);
            bool framed = (cd->nmcd.uItemState & (CDIS_FOCUS | CDIS_HOT)) ||
                          ListView_GetItemState(lv_, row, LVIS_SELECTED);
            RECT r{};
            if (!framed && ListView_GetItemRect(lv_, row, &r, LVIR_BOUNDS)) {
                r.top = r.bottom - 1;
                COLORREF old = SetDCBrushColor(cd->nmcd.hdc, lineColor_);
                FillRect(cd->nmcd.hdc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
                SetDCBrushColor(cd->nmcd.hdc, old);
            }
        }
        return true;
    }
    case LVN_ODFINDITEMW:
        *result = -1;   // typing goes to the filter box instead
        return true;
    }
    return false;
}
