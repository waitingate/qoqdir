//! The file list: a virtual ListView that asks for each visible row's text, so 100,000 files show at once.
//! Row i is items[view[i]] (view: the items that pass the filter). Only `update` changes them, and it
//! clears the selection with them, so no row or selection can point at the wrong file. The model is
//! never borrowed while a message goes to the ListView, which can call straight back into `on_notify`.

use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::ffi::{OsStr, OsString};
use std::path::PathBuf;

use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, WPARAM};
use windows::Win32::Graphics::Gdi::InvalidateRect;
use windows::Win32::Storage::FileSystem::FILE_ATTRIBUTE_HIDDEN;
use windows::Win32::UI::Controls::{
    HDI_FORMAT, HDITEMW, HDS_FLAT, HDF_SORTDOWN, HDF_SORTUP, HDM_GETITEMW, HDM_SETITEMW, LIST_VIEW_ITEM_STATE_FLAGS,
    LVCF_FMT, LVCF_TEXT, LVCF_WIDTH, LVCFMT_LEFT, LVCFMT_RIGHT, LVCOLUMNW, LVIF_IMAGE, LVIF_TEXT, LVIR_BOUNDS,
    LVIS_FOCUSED, LVIS_SELECTED, LVITEMW, LVM_CANCELEDITLABEL, LVM_EDITLABELW, LVM_ENSUREVISIBLE, LVM_GETCOLUMNWIDTH,
    LVM_GETEDITCONTROL, LVM_GETHEADER, LVM_GETITEMRECT, LVM_GETNEXTITEM, LVM_GETTOPINDEX, LVM_INSERTCOLUMNW,
    LVM_SCROLL, LVM_SETCOLUMNWIDTH, LVM_SETEXTENDEDLISTVIEWSTYLE, LVM_SETIMAGELIST, LVM_SETITEMCOUNT,
    LVM_SETITEMSTATE, LVM_SETSELECTIONMARK, LVN_BEGINLABELEDITW, LVN_COLUMNCLICK, LVN_GETDISPINFOW,
    LVN_ODFINDITEMW, LVNI_FOCUSED, LVNI_SELECTED, LVS_EDITLABELS, LVS_EX_DOUBLEBUFFER, LVS_EX_FULLROWSELECT,
    LVS_OWNERDATA, LVS_REPORT, LVS_SHAREIMAGELISTS, LVS_SHOWSELALWAYS, LVSIL_SMALL, NMHDR, NMLISTVIEW,
    NMLVDISPINFOW, EM_SETSEL, HEADER_CONTROL_FORMAT_FLAGS,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, GWL_STYLE, GetWindowLongPtrW, HMENU, PostMessageW, SetWindowLongPtrW, WINDOW_EX_STYLE, WINDOW_STYLE,
    WS_CHILD, WS_TABSTOP, WS_VISIBLE,
};

use windows::core::w;

use crate::folder::{Item, Listing, Location, SortKey, equal_no_case, is_shortcut, matches_filter, sort_items, to_lower, format_size, format_time};
use crate::shell::{IconBatch, has_own_icon, set_theme, system_image_list, type_icon, type_name};
use crate::util::{copy_to, focus, send, wide};

const COLUMNS: [&str; 4] = ["Name", "Date modified", "Type", "Size"];
const LVSICF_NOSCROLL: isize = 2;

struct Model {
    location: Location,
    items: Vec<Item>,
    view: Vec<usize>,
    filter: Vec<u16>,  // NUL-terminated
    show_hidden: bool, // hidden items are always read; they are listed only while this is on
    total: usize,      // the items a filter would narrow down (not the hidden ones)
    key: SortKey,
    ascending: bool,
}

impl Model {
    /// The item at a row, or None past the end.
    fn item(&self, row: usize) -> Option<&Item> {
        self.view.get(row).map(|&i| &self.items[i])
    }
}

pub struct FileList {
    hwnd: HWND,
    model: RefCell<Model>,
    own_icons: RefCell<HashMap<PathBuf, i32>>, // full path -> own icon, for the folder shown
}

impl FileList {
    // ------------------------------------------------------------ contents

    pub fn create(parent: HWND, id: i32) -> FileList {
        let style = WS_CHILD.0 | WS_VISIBLE.0 | WS_TABSTOP.0 | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS
            | LVS_EDITLABELS | LVS_SHAREIMAGELISTS;
        let hwnd = unsafe {
            CreateWindowExW(
                WINDOW_EX_STYLE(0),
                w!("SysListView32"),
                w!(""),
                WINDOW_STYLE(style),
                0,
                0,
                0,
                0,
                Some(parent),
                Some(HMENU(id as isize as _)),
                None,
                None,
            )
        }
        .unwrap_or_default();
        let ex = (LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER) as isize;
        send(hwnd, LVM_SETEXTENDEDLISTVIEWSTYLE, ex as usize, ex);
        set_theme(hwnd, "Explorer");
        // The column header in Windows' plain flat look: a light gray that goes with the list's (the themed one is white).
        let header = HWND(send(hwnd, LVM_GETHEADER, 0, 0) as _);
        set_theme(header, "");
        unsafe { SetWindowLongPtrW(header, GWL_STYLE, GetWindowLongPtrW(header, GWL_STYLE) | HDS_FLAT as isize) };
        send(hwnd, LVM_SETIMAGELIST, LVSIL_SMALL as usize, system_image_list().0);
        for (i, name) in COLUMNS.iter().enumerate() {
            let text = wide(name);
            let col = LVCOLUMNW {
                mask: LVCF_TEXT | LVCF_WIDTH | LVCF_FMT,
                fmt: if i == 3 { LVCFMT_RIGHT } else { LVCFMT_LEFT },
                cx: 100,
                pszText: windows::core::PWSTR(text.as_ptr() as *mut u16),
                ..Default::default()
            };
            send(hwnd, LVM_INSERTCOLUMNW, i, &col as *const _ as isize);
        }
        let list = FileList {
            hwnd,
            own_icons: RefCell::new(HashMap::new()),
            model: RefCell::new(Model {
                location: Location::ThisPc,
                items: Vec::new(),
                view: Vec::new(),
                filter: vec![0],
                show_hidden: false,
                total: 0,
                key: SortKey::Name,
                ascending: true,
            }),
        };
        list.show_sort_arrow();
        list
    }

    pub fn hwnd(&self) -> HWND {
        self.hwnd
    }

    /// New folder contents; keeps sort and filter.
    pub fn set_listing(&self, listing: Listing) {
        self.update(false, |m| {
            m.location = listing.location;
            m.items = listing.items;
            for it in &mut m.items {
                // Cached per extension, so this is quick; lets Type sort by what is shown.
                if it.type_text.is_none() {
                    it.type_text = Some(type_name(it));
                }
            }
            sort_items(&mut m.items, m.key, m.ascending);
        });
    }

    /// The folder's own icons (a .exe, a .lnk), read after its listing. A late batch for a folder no longer shown is of
    /// no use. A batch replaces the last one: the map never outgrows a folder, and a program replaced by another gets
    /// its new icon on the next read.
    pub fn set_icons(&self, batch: &IconBatch) {
        if !self.location().same(&batch.dir) {
            return;
        }
        *self.own_icons.borrow_mut() = batch.icons.iter().map(|(name, icon)| (batch.dir.join(name), *icon)).collect();
        unsafe {
            let _ = InvalidateRect(Some(self.hwnd), None, false);
        }
    }

    pub fn set_filter(&self, filter: &OsStr) {
        let w = wide(filter);
        if self.model.borrow().filter == w {
            return;
        }
        self.update(true, |m| m.filter = w);
    }

    pub fn set_show_hidden(&self, show: bool) {
        self.update(true, |m| m.show_hidden = show);
    }

    pub fn show_hidden(&self) -> bool {
        self.model.borrow().show_hidden
    }

    pub fn set_sort(&self, key: SortKey, ascending: bool) {
        self.update(true, |m| {
            m.key = key;
            m.ascending = ascending;
            sort_items(&mut m.items, key, ascending);
        });
        self.show_sort_arrow();
    }

    pub fn sort(&self) -> (SortKey, bool) {
        let m = self.model.borrow();
        (m.key, m.ascending)
    }

    /// The only way the model changes: `view` is rebuilt in the same borrow, and the selection reset with it.
    fn update(&self, keep_selection: bool, change: impl FnOnce(&mut Model)) {
        let (names, focus) = if keep_selection { (self.selected_names(), self.focused_name()) } else { (Vec::new(), None) };
        // The edit box knows a row, not a file: close it before rows move, or Enter renames whatever lands there.
        if self.renaming() {
            send(self.hwnd, LVM_CANCELEDITLABEL, 0, 0);
        }
        let count = {
            let mut m = self.model.borrow_mut();
            change(&mut m);
            let m = &mut *m;
            let listed: Vec<usize> =
                (0..m.items.len()).filter(|&i| m.show_hidden || m.items[i].attrs & FILE_ATTRIBUTE_HIDDEN.0 == 0).collect();
            m.total = listed.len();
            m.view = listed.into_iter().filter(|&i| matches_filter(&m.items[i], &m.filter)).collect();
            m.view.len()
        };
        self.set_state(None, LIST_VIEW_ITEM_STATE_FLAGS(0), LIST_VIEW_ITEM_STATE_FLAGS(LVIS_SELECTED.0 | LVIS_FOCUSED.0));
        send(self.hwnd, LVM_SETITEMCOUNT, count, LVSICF_NOSCROLL);
        unsafe {
            let _ = InvalidateRect(Some(self.hwnd), None, false);
        }
        self.select(&names, focus.as_deref());
        // select() left the focus on a row; the old scroll position may be past the end.
        self.ensure_visible(self.focused());
    }

    pub fn set_state(&self, row: Option<usize>, state: LIST_VIEW_ITEM_STATE_FLAGS, mask: LIST_VIEW_ITEM_STATE_FLAGS) {
        let item = LVITEMW { state, stateMask: mask, ..Default::default() };
        send(self.hwnd, LVM_SETITEMSTATE, row.unwrap_or(usize::MAX), &item as *const _ as isize);
    }

    pub fn location(&self) -> Location {
        self.model.borrow().location.clone()
    }
    pub fn count(&self) -> usize {
        self.model.borrow().view.len()
    }
    /// The items a filter would narrow down (not the hidden ones).
    pub fn total(&self) -> usize {
        self.model.borrow().total
    }
    pub fn item(&self, row: usize) -> Option<Item> {
        self.model.borrow().item(row).cloned()
    }

    // ------------------------------------------------------------ what is shown

    /// A subfolder of that name, shown or filtered out.
    pub fn has_folder(&self, name: &OsStr) -> bool {
        self.model.borrow().items.iter().any(|it| it.is_dir() && equal_no_case(&it.name, name))
    }
    /// The subfolders' and shortcuts' names (not the hidden ones), sorted: what the tree may show.
    pub fn tree_names(&self) -> Vec<OsString> {
        let m = self.model.borrow();
        let shown = |it: &Item| (it.is_dir() || is_shortcut(&it.name)) && it.attrs & FILE_ATTRIBUTE_HIDDEN.0 == 0;
        let mut names: Vec<OsString> = m.items.iter().filter(|it| shown(it)).map(|it| it.name.clone()).collect();
        names.sort();
        names
    }
    /// How many of these rows are files (not folders), and their total size: for the status bar.
    pub fn file_sizes(&self, rows: &[usize]) -> (usize, u64) {
        let m = self.model.borrow();
        let files = rows.iter().filter_map(|&r| m.item(r)).filter(|it| !it.is_dir());
        files.fold((0, 0), |(n, bytes), it| (n + 1, bytes + it.size))
    }
    /// The item at a row, as a location of its own (for shell commands).
    pub fn item_location(&self, row: usize) -> Option<Location> {
        let m = self.model.borrow();
        m.item(row).map(|it| Location::dir(m.location.join(&it.name)))
    }

    /// An item's own icon if it has been read by now, else its type's.
    fn icon_of(&self, m: &Model, it: &Item) -> i32 {
        if it.icon >= 0 {
            return it.icon;
        }
        let own = if has_own_icon(it) { self.own_icons.borrow().get(&m.location.join(&it.name)).copied() } else { None };
        own.unwrap_or_else(|| type_icon(it))
    }

    pub fn selection(&self) -> Vec<usize> {
        let count = self.count();
        let mut rows = Vec::new();
        let mut i = send(self.hwnd, LVM_GETNEXTITEM, usize::MAX, LVNI_SELECTED as isize);
        while i >= 0 {
            if (i as usize) < count {
                rows.push(i as usize);
            }
            i = send(self.hwnd, LVM_GETNEXTITEM, i as usize, LVNI_SELECTED as isize);
        }
        rows
    }

    /// With their attributes, size and date: see `shell::data_object`.
    pub fn selected_items(&self) -> Vec<Item> {
        let rows = self.selection();
        let m = self.model.borrow();
        rows.into_iter().filter_map(|r| m.item(r).cloned()).collect()
    }

    pub fn selected_names(&self) -> Vec<OsString> {
        let rows = self.selection();
        let m = self.model.borrow();
        rows.into_iter().filter_map(|r| m.item(r).map(|it| it.name.clone())).collect()
    }

    pub fn focused(&self) -> Option<usize> {
        let row = send(self.hwnd, LVM_GETNEXTITEM, usize::MAX, LVNI_FOCUSED as isize);
        (row >= 0 && (row as usize) < self.count()).then_some(row as usize)
    }

    pub fn focused_name(&self) -> Option<OsString> {
        let row = self.focused()?; // asks the control: before the borrow
        let m = self.model.borrow();
        m.item(row).map(|it| it.name.clone())
    }

    // ------------------------------------------------------------ selection and scrolling

    /// Selects the named items (case-insensitive) and returns the row of `focus`.
    pub fn select(&self, names: &[OsString], focus: Option<&OsStr>) -> Option<usize> {
        self.set_state(None, LIST_VIEW_ITEM_STATE_FLAGS(0), LIST_VIEW_ITEM_STATE_FLAGS(LVIS_SELECTED.0 | LVIS_FOCUSED.0));
        let (rows, found) = {
            let m = self.model.borrow();
            let wanted: HashSet<Vec<u16>> = names.iter().map(|n| to_lower(n)).collect();
            let focus = focus.map(to_lower);
            let mut rows = Vec::new();
            let mut focus_row = None;
            if !wanted.is_empty() || focus.is_some() {
                for (row, &i) in m.view.iter().enumerate() {
                    let name = to_lower(&m.items[i].name);
                    if wanted.contains(&name) {
                        rows.push(row);
                    }
                    if focus_row.is_none() && focus.as_ref() == Some(&name) {
                        focus_row = Some(row);
                    }
                }
            }
            (rows, focus_row)
        };
        // All of them (Ctrl+A, then a sort or a filter key) in one message, not one per row and its notification.
        if !rows.is_empty() && rows.len() == self.count() {
            self.select_all();
        } else {
            for row in rows {
                self.set_state(Some(row), LVIS_SELECTED, LVIS_SELECTED);
            }
        }
        self.focus_row(found.unwrap_or(0)); // something always has keyboard focus, so arrow keys work right away
        found
    }

    /// Gives `row` the keyboard focus (the selection stays); returns it, or None if there is no such row.
    pub fn focus_row(&self, row: usize) -> Option<usize> {
        if row >= self.count() {
            return None;
        }
        self.set_state(None, LIST_VIEW_ITEM_STATE_FLAGS(0), LVIS_FOCUSED);
        self.set_state(Some(row), LVIS_FOCUSED, LVIS_FOCUSED);
        send(self.hwnd, LVM_SETSELECTIONMARK, 0, row as isize);
        Some(row)
    }

    pub fn select_all(&self) {
        self.set_state(None, LVIS_SELECTED, LVIS_SELECTED);
    }

    pub fn top_row(&self) -> usize {
        send(self.hwnd, LVM_GETTOPINDEX, 0, 0).max(0) as usize
    }

    pub fn scroll_to_row(&self, row: usize) {
        if self.count() == 0 {
            return;
        }
        let mut r = windows::Win32::Foundation::RECT { left: LVIR_BOUNDS as i32, ..Default::default() };
        if send(self.hwnd, LVM_GETITEMRECT, 0, &mut r as *mut _ as isize) == 0 {
            return;
        }
        let dy = (row as i32 - self.top_row() as i32) * (r.bottom - r.top);
        send(self.hwnd, LVM_SCROLL, 0, dy as isize);
    }

    pub fn ensure_visible(&self, row: Option<usize>) {
        if let Some(row) = row.filter(|&r| r < self.count()) {
            send(self.hwnd, LVM_ENSUREVISIBLE, row, 0);
        }
    }

    // ------------------------------------------------------------ renaming and columns

    pub fn rename(&self, row: Option<usize>) {
        let Some(row) = row.filter(|&r| r < self.count()) else { return };
        if matches!(self.location(), Location::ThisPc) {
            return;
        }
        focus(self.hwnd);
        send(self.hwnd, LVM_EDITLABELW, row, 0);
    }

    pub fn renaming(&self) -> bool {
        self.edit_control().is_some()
    }

    pub fn edit_control(&self) -> Option<HWND> {
        let h = send(self.hwnd, LVM_GETEDITCONTROL, 0, 0);
        (h != 0).then_some(HWND(h as _))
    }

    pub fn column_widths(&self) -> Vec<i32> {
        (0..COLUMNS.len()).map(|i| send(self.hwnd, LVM_GETCOLUMNWIDTH, i, 0) as i32).collect()
    }

    pub fn set_column_widths(&self, widths: &[i32]) {
        for (i, &w) in widths.iter().enumerate().take(COLUMNS.len()) {
            send(self.hwnd, LVM_SETCOLUMNWIDTH, i, w as isize);
        }
    }

    fn show_sort_arrow(&self) {
        let (key, asc) = self.sort();
        let header = HWND(send(self.hwnd, LVM_GETHEADER, 0, 0) as _);
        for i in 0..COLUMNS.len() {
            let mut hd = HDITEMW { mask: HDI_FORMAT, ..Default::default() };
            send(header, HDM_GETITEMW, i, &mut hd as *mut _ as isize);
            let mut fmt = hd.fmt.0 & !(HDF_SORTUP.0 | HDF_SORTDOWN.0);
            if i == key as usize {
                fmt |= if asc { HDF_SORTUP.0 } else { HDF_SORTDOWN.0 };
            }
            hd.fmt = HEADER_CONTROL_FORMAT_FLAGS(fmt);
            send(header, HDM_SETITEMW, i, &hd as *const _ as isize);
        }
    }

    // ------------------------------------------------------------ notifications

    /// Handles display-only notifications. Returns None for ones the app handles.
    ///
    /// # Safety
    /// `nm` must be the NMHDR of a WM_NOTIFY from this list.
    pub unsafe fn on_notify(&self, nm: *const NMHDR) -> Option<LRESULT> {
        let code = unsafe { (*nm).code };
        match code {
            LVN_GETDISPINFOW => {
                let lvi = unsafe { &mut (*(nm as *mut NMLVDISPINFOW)).item };
                let m = self.model.borrow(); // held while painting: nothing below sends the list a message
                let Some(it) = m.item(lvi.iItem as usize) else { return Some(LRESULT(0)) };
                if lvi.mask.contains(LVIF_TEXT) {
                    let text: OsString = match lvi.iSubItem {
                        0 => it.shown().to_os_string(),
                        1 => format_time(it.modified),
                        2 => type_name(it),
                        3 if !it.is_dir() => format_size(it.size),
                        _ => OsString::new(),
                    };
                    unsafe { copy_to(lvi.pszText.0, lvi.cchTextMax, &text) };
                }
                if lvi.mask.contains(LVIF_IMAGE) {
                    lvi.iImage = self.icon_of(&m, it); // not stored in the item: a file's own icon may come later
                }
                Some(LRESULT(0))
            }
            LVN_COLUMNCLICK => {
                let key = SortKey::from_index(unsafe { (*(nm as *const NMLISTVIEW)).iSubItem });
                let (cur, asc) = self.sort();
                // Names and types read best A-Z; dates and sizes newest/largest first.
                let asc = if key == cur { !asc } else { matches!(key, SortKey::Name | SortKey::Type) };
                self.set_sort(key, asc);
                Some(LRESULT(0))
            }
            LVN_BEGINLABELEDITW => {
                let row = unsafe { (*(nm as *const NMLVDISPINFOW)).item.iItem } as usize;
                let Some(it) = self.item(row).filter(|_| matches!(self.location(), Location::Dir(_))) else {
                    return Some(LRESULT(1)); // drives cannot be renamed here
                };
                // Select the name without its extension, like Explorer.
                let name: Vec<u16> = wide(&it.name);
                let dot = if it.is_dir() { None } else { name.iter().rposition(|&c| c == b'.' as u16) };
                if let (Some(dot), Some(edit)) = (dot.filter(|&d| d > 0), self.edit_control()) {
                    unsafe {
                        let _ = PostMessageW(Some(edit), EM_SETSEL, WPARAM(0), LPARAM(dot as isize));
                    }
                }
                Some(LRESULT(0))
            }
            LVN_ODFINDITEMW => Some(LRESULT(-1)), // typing goes to the filter box instead
            _ => None,
        }
    }
}
