//! The file list: a "virtual" ListView that stores no items itself. It asks for
//! the text of each visible row, so a folder with 100,000 files shows instantly.
//!
//! `items` holds the folder, `view` holds the indices that pass the filter.
//! Row i on screen is always items[view[i]]; `update` is the only place that
//! changes `view` (together with `items`), and it always clears the ListView's
//! selection with it, so no row or selection can point at the wrong file.
//!
//! Borrow rule: the model is never borrowed while a message is sent to the
//! ListView, because Windows may call straight back into `on_notify`.

use std::cell::RefCell;
use std::collections::HashSet;
use std::ffi::{OsStr, OsString};

use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, WPARAM};
use windows::Win32::Graphics::Gdi::InvalidateRect;
use windows::Win32::Storage::FileSystem::FILE_ATTRIBUTE_HIDDEN;
use windows::Win32::UI::Controls::{
    HDI_FORMAT, HDITEMW, HDF_SORTDOWN, HDF_SORTUP, HDM_GETITEMW, HDM_SETITEMW, LIST_VIEW_ITEM_STATE_FLAGS,
    LVCF_FMT, LVCF_TEXT, LVCF_WIDTH, LVCFMT_LEFT, LVCFMT_RIGHT, LVCOLUMNW, LVIF_IMAGE, LVIF_TEXT, LVIR_BOUNDS,
    LVIS_FOCUSED, LVIS_SELECTED, LVITEMW, LVM_CANCELEDITLABEL, LVM_EDITLABELW, LVM_ENSUREVISIBLE, LVM_GETCOLUMNWIDTH,
    LVM_GETEDITCONTROL, LVM_GETHEADER, LVM_GETITEMRECT, LVM_GETNEXTITEM, LVM_GETTOPINDEX, LVM_INSERTCOLUMNW,
    LVM_SCROLL, LVM_SETCOLUMNWIDTH, LVM_SETEXTENDEDLISTVIEWSTYLE, LVM_SETIMAGELIST, LVM_SETITEMCOUNT,
    LVM_SETITEMSTATE, LVM_SETSELECTIONMARK, LVN_BEGINLABELEDITW, LVN_COLUMNCLICK, LVN_GETDISPINFOW,
    LVN_ODFINDITEMW, LVNI_FOCUSED, LVNI_SELECTED, LVS_EDITLABELS, LVS_EX_DOUBLEBUFFER, LVS_EX_FULLROWSELECT,
    LVS_OWNERDATA, LVS_REPORT, LVS_SHAREIMAGELISTS, LVS_SHOWSELALWAYS, LVSIL_SMALL, NMHDR, NMLISTVIEW,
    NMLVDISPINFOW, SetWindowTheme, EM_SETSEL, HEADER_CONTROL_FORMAT_FLAGS,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, HMENU, PostMessageW, WINDOW_EX_STYLE, WINDOW_STYLE, WS_CHILD,
    WS_TABSTOP, WS_VISIBLE,
};
use windows::Win32::UI::Input::KeyboardAndMouse::SetFocus;
use windows::core::w;

use crate::folder::{Item, Listing, Location, SortKey, equal_no_case, matches_filter, sort_items, to_lower, format_size, format_time};
use crate::shell::{icon_for, system_image_list, type_name};
use crate::util::{send, copy_to, wide};

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

pub struct FileList {
    hwnd: HWND,
    model: RefCell<Model>,
}


impl FileList {
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
        unsafe {
            let _ = SetWindowTheme(hwnd, w!("Explorer"), None);
        }
        send(hwnd, LVM_SETIMAGELIST, LVSIL_SMALL as usize, system_image_list().0 as isize);
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
        self.ensure_visible(Some(0));
    }

    pub fn set_filter(&self, filter: &OsStr) {
        // A run of stars means one star; the matcher would try every way of splitting the name between them.
        let mut w = wide(filter);
        w.dedup_by(|a, b| *a == b'*' as u16 && *b == b'*' as u16);
        if self.model.borrow().filter == w {
            return;
        }
        self.update(true, |m| m.filter = w);
        self.ensure_visible(self.focused().or(Some(0))); // the old scroll position may be past the end
    }

    pub fn set_show_hidden(&self, show: bool) {
        self.update(true, |m| m.show_hidden = show);
        self.ensure_visible(self.focused().or(Some(0)));
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
        self.ensure_visible(self.focused());
    }

    pub fn sort(&self) -> (SortKey, bool) {
        let m = self.model.borrow();
        (m.key, m.ascending)
    }

    /// The only way the model changes: `change` edits it and `view` is rebuilt in
    /// the same borrow, so Windows can never see a row that points at the wrong
    /// (or a missing) item. The ListView's selection is reset with it.
    fn update(&self, keep_selection: bool, change: impl FnOnce(&mut Model)) {
        let (names, focus) = if keep_selection { (self.selected_names(), self.focused_name()) } else { (Vec::new(), None) };
        // The edit box remembers a row number, not a file: close it before the rows move, or Enter
        // would rename whatever lands on that row (a navigation, a new sort or a filter under it).
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
    }

    fn set_state(&self, row: Option<usize>, state: LIST_VIEW_ITEM_STATE_FLAGS, mask: LIST_VIEW_ITEM_STATE_FLAGS) {
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
        let m = self.model.borrow();
        m.view.get(row).map(|&i| m.items[i].clone())
    }
    /// A subfolder of that name, shown or filtered out.
    pub fn has_folder(&self, name: &OsStr) -> bool {
        self.model.borrow().items.iter().any(|it| it.is_dir() && equal_no_case(&it.name, name))
    }
    /// The item at a row, as a location of its own (for shell commands).
    pub fn item_location(&self, row: usize) -> Option<Location> {
        let m = self.model.borrow();
        m.view.get(row).map(|&i| Location::dir(m.location.join(&m.items[i].name)))
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
        rows.into_iter().filter_map(|r| m.view.get(r).map(|&i| m.items[i].clone())).collect()
    }

    pub fn selected_names(&self) -> Vec<OsString> {
        let rows = self.selection();
        let m = self.model.borrow();
        rows.into_iter().filter_map(|r| m.view.get(r).map(|&i| m.items[i].name.clone())).collect()
    }

    pub fn focused(&self) -> Option<usize> {
        let row = send(self.hwnd, LVM_GETNEXTITEM, usize::MAX, LVNI_FOCUSED as isize);
        (row >= 0 && (row as usize) < self.count()).then_some(row as usize)
    }

    pub fn focused_name(&self) -> Option<OsString> {
        self.focused().and_then(|r| self.item(r)).map(|it| it.name)
    }

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
        for row in rows {
            self.set_state(Some(row), LVIS_SELECTED, LVIS_SELECTED);
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

    pub fn rename(&self, row: Option<usize>) {
        let Some(row) = row.filter(|&r| r < self.count()) else { return };
        if matches!(self.location(), Location::ThisPc) {
            return;
        }
        unsafe {
            let _ = SetFocus(Some(self.hwnd));
        }
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

    /// Handles display-only notifications. Returns None for ones the app handles.
    ///
    /// # Safety
    /// `nm` must be the NMHDR of a WM_NOTIFY from this list.
    pub unsafe fn on_notify(&self, nm: *const NMHDR) -> Option<LRESULT> {
        let code = unsafe { (*nm).code };
        match code {
            LVN_GETDISPINFOW => {
                let lvi = unsafe { &mut (*(nm as *mut NMLVDISPINFOW)).item };
                let row = lvi.iItem as usize;
                let (Some(it), location) = (self.item(row), self.location()) else { return Some(LRESULT(0)) };
                if lvi.mask.contains(LVIF_TEXT) {
                    let text: OsString = match lvi.iSubItem {
                        0 => it.shown().to_os_string(),
                        1 => format_time(it.modified),
                        2 => type_name(&it),
                        3 if !it.is_dir() => format_size(it.size),
                        _ => OsString::new(),
                    };
                    unsafe { copy_to(lvi.pszText.0, lvi.cchTextMax, &text) };
                }
                if lvi.mask.contains(LVIF_IMAGE) {
                    // Not stored in the item: a file's own icon may arrive later.
                    lvi.iImage = if it.icon >= 0 { it.icon } else { icon_for(&location.join(&it.name), &it) };
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
