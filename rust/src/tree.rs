//! The folder tree on the left. Subfolders are read only when a node is expanded,
//! on a worker thread, so a slow or disconnected network drive never freezes the window.

use std::cell::{Cell, RefCell};
use std::collections::HashSet;
use std::ffi::OsString;

use windows::Win32::Foundation::{HWND, LPARAM, POINT, WPARAM};
use windows::Win32::Graphics::Gdi::ScreenToClient;
use windows::Win32::System::Com::{COINIT_APARTMENTTHREADED, COINIT_DISABLE_OLE1DDE, CoInitializeEx, CoUninitialize};
use windows::Win32::UI::Controls::{
    HTREEITEM, NM_CLICK, NMHDR, NMTREEVIEWW, SetWindowTheme, TVE_EXPAND, TVGN_CARET, TVGN_CHILD, TVHITTESTINFO,
    TVHT_ONITEM, TVI_LAST, TVI_ROOT, TVIF_CHILDREN, TVIF_IMAGE, TVIF_PARAM, TVIF_SELECTEDIMAGE, TVIF_TEXT,
    TVINSERTSTRUCTW, TVINSERTSTRUCTW_0, TVITEMEXW, TVITEMEXW_CHILDREN, TVITEMW, TVM_DELETEITEM, TVM_EXPAND, TVM_GETITEMW,
    TVM_GETNEXTITEM, TVM_HITTEST, TVM_INSERTITEMW, TVM_SETEXTENDEDSTYLE, TVM_SETIMAGELIST, TVM_SETITEMW,
    TVN_ITEMEXPANDINGW, TVN_SELCHANGEDW, TVS_EX_DOUBLEBUFFER, TVS_EX_FADEINOUTEXPANDOS, TVS_FULLROWSELECT,
    TVS_HASBUTTONS, TVS_NOHSCROLL, TVS_SHOWSELALWAYS, TVS_TRACKSELECT, TVSIL_NORMAL, TVC_BYKEYBOARD, TVC_BYMOUSE,
};
use windows::Win32::UI::Shell::{
    FOLDERID_Desktop, FOLDERID_Documents, FOLDERID_Downloads, FOLDERID_Music, FOLDERID_Pictures, FOLDERID_Videos,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, GetCursorPos, HMENU, PostMessageW, WINDOW_EX_STYLE, WINDOW_STYLE, WS_CHILD,
    WS_TABSTOP, WS_VISIBLE,
};
use windows::core::{PWSTR, w};

use crate::folder::{Location, SortKey, list_folder, sort_items};
use crate::shell::{describe, folder_icon, known_folder, system_image_list};
use crate::util::{send, wide};

/// Subfolders of one node, read on a worker thread and posted to the tree's parent window.
pub struct TreeBatch {
    fill: u32, // which fill() the node belongs to; batches for an older tree are dropped
    node: isize,
    children: Vec<(Location, OsString, Option<i32>)>, // icon None: plain folder icon
}

pub struct FolderTree {
    hwnd: HWND,
    parent: HWND,
    batch_msg: u32,
    fill: Cell<u32>,
    paths: RefCell<Vec<Location>>, // node lParam indexes this
    reading: RefCell<HashSet<isize>>, // nodes whose subfolders are being read
}


/// Reads the subfolders of a location. Runs on a worker thread, so it may block.
fn read_children(location: &Location) -> Vec<(Location, OsString, Option<i32>)> {
    match location {
        Location::ThisPc => {
            let mask = unsafe { windows::Win32::Storage::FileSystem::GetLogicalDrives() };
            (0..26u8)
                .filter(|i| mask & (1 << i) != 0)
                .map(|i| {
                    let drive = Location::dir(format!("{}:\\", (b'A' + i) as char));
                    let (name, icon) = describe(&drive);
                    (drive, name, Some(icon))
                })
                .collect()
        }
        Location::Dir(_) => {
            let mut dirs: Vec<_> = list_folder(location, false).items.into_iter().filter(|i| i.is_dir()).collect();
            sort_items(&mut dirs, SortKey::Name, true);
            dirs.into_iter().map(|it| (Location::dir(location.join(&it.name)), it.name, None)).collect()
        }
    }
}

impl FolderTree {
    /// `batch_msg` is posted to `parent` with a `Box<TreeBatch>` (for `add_children`)
    /// when subfolders have been read.
    pub fn create(parent: HWND, id: i32, batch_msg: u32) -> FolderTree {
        let style = WS_CHILD.0 | WS_VISIBLE.0 | WS_TABSTOP.0 | TVS_HASBUTTONS | TVS_SHOWSELALWAYS | TVS_TRACKSELECT
            | TVS_FULLROWSELECT | TVS_NOHSCROLL;
        let hwnd = unsafe {
            CreateWindowExW(
                WINDOW_EX_STYLE(0),
                w!("SysTreeView32"),
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
        let ex = (TVS_EX_DOUBLEBUFFER | TVS_EX_FADEINOUTEXPANDOS) as isize;
        send(hwnd, TVM_SETEXTENDEDSTYLE, ex as usize, ex);
        unsafe {
            let _ = SetWindowTheme(hwnd, w!("Explorer"), None);
        }
        send(hwnd, TVM_SETIMAGELIST, TVSIL_NORMAL as usize, system_image_list().0 as isize);
        FolderTree {
            hwnd,
            parent,
            batch_msg,
            fill: Cell::new(0),
            paths: RefCell::new(Vec::new()),
            reading: RefCell::new(HashSet::new()),
        }
    }

    pub fn hwnd(&self) -> HWND {
        self.hwnd
    }

    /// Rebuilds the roots: the user folders, then This PC with its drives.
    pub fn fill(&self) {
        self.fill.set(self.fill.get() + 1); // results still being read for the old nodes will be dropped
        self.reading.borrow_mut().clear();
        send(self.hwnd, TVM_DELETEITEM, 0, TVI_ROOT.0);
        self.paths.borrow_mut().clear();
        for id in [&FOLDERID_Desktop, &FOLDERID_Downloads, &FOLDERID_Documents, &FOLDERID_Pictures, &FOLDERID_Music, &FOLDERID_Videos] {
            if let Some(path) = known_folder(id) {
                let location = Location::dir(path);
                let (name, icon) = describe(&location);
                self.add(TVI_ROOT, location, &name, icon);
            }
        }
        let (_, icon) = describe(&Location::ThisPc);
        let this_pc = self.add(TVI_ROOT, Location::ThisPc, &"This PC".into(), icon);
        send(self.hwnd, TVM_EXPAND, TVE_EXPAND.0 as usize, this_pc.0);
    }

    fn add(&self, parent: HTREEITEM, location: Location, text: &OsString, icon: i32) -> HTREEITEM {
        let index = {
            let mut paths = self.paths.borrow_mut();
            paths.push(location);
            paths.len() - 1
        };
        let text = wide(text);
        let ins = TVINSERTSTRUCTW {
            hParent: parent,
            hInsertAfter: TVI_LAST,
            Anonymous: TVINSERTSTRUCTW_0 {
                itemex: TVITEMEXW {
                    mask: TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_CHILDREN,
                    pszText: PWSTR(text.as_ptr() as *mut u16),
                    iImage: icon,
                    iSelectedImage: icon,
                    lParam: LPARAM(index as isize),
                    cChildren: TVITEMEXW_CHILDREN(1), // unknown until expanded
                    ..Default::default()
                },
            },
        };
        HTREEITEM(send(self.hwnd, TVM_INSERTITEMW, 0, &ins as *const _ as isize))
    }

    fn location_of(&self, node: HTREEITEM) -> Option<Location> {
        let mut item = TVITEMW { mask: TVIF_PARAM, hItem: node, ..Default::default() };
        if send(self.hwnd, TVM_GETITEMW, 0, &mut item as *mut _ as isize) == 0 {
            return None;
        }
        self.paths.borrow().get(item.lParam.0 as usize).cloned()
    }

    fn start_reading(&self, node: HTREEITEM) {
        if send(self.hwnd, TVM_GETNEXTITEM, TVGN_CHILD as usize, node.0) != 0 || !self.reading.borrow_mut().insert(node.0) {
            return; // already read, or being read
        }
        let Some(location) = self.location_of(node) else { return };
        let (parent, msg, fill, node) = (self.parent.0 as usize, self.batch_msg, self.fill.get(), node.0);
        std::thread::spawn(move || unsafe {
            let co = CoInitializeEx(None, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            let batch = Box::into_raw(Box::new(TreeBatch { fill, node, children: read_children(&location) }));
            if PostMessageW(Some(HWND(parent as _)), msg, WPARAM(0), LPARAM(batch as isize)).is_err() {
                drop(Box::from_raw(batch)); // the window is gone
            }
            if co.is_ok() {
                CoUninitialize();
            }
        });
    }

    pub fn add_children(&self, batch: TreeBatch) {
        // Nodes are only ever deleted by fill(), so a batch from the current fill has a valid node.
        if batch.fill != self.fill.get() || !self.reading.borrow_mut().remove(&batch.node) {
            return;
        }
        let node = HTREEITEM(batch.node);
        if batch.children.is_empty() {
            let item = TVITEMW { mask: TVIF_CHILDREN, hItem: node, cChildren: TVITEMEXW_CHILDREN(0), ..Default::default() };
            send(self.hwnd, TVM_SETITEMW, 0, &item as *const _ as isize);
            return;
        }
        for (location, name, icon) in batch.children {
            self.add(node, location, &name, icon.unwrap_or_else(folder_icon));
        }
        send(self.hwnd, TVM_EXPAND, TVE_EXPAND.0 as usize, node.0);
    }

    /// The location under a screen point, or of the selected node for (-1, -1).
    pub fn location_at(&self, pt: POINT) -> Option<Location> {
        let node = if pt.x == -1 && pt.y == -1 {
            HTREEITEM(send(self.hwnd, TVM_GETNEXTITEM, TVGN_CARET as usize, 0))
        } else {
            let mut hit = TVHITTESTINFO { pt, ..Default::default() };
            unsafe {
                let _ = ScreenToClient(self.hwnd, &mut hit.pt);
            }
            HTREEITEM(send(self.hwnd, TVM_HITTEST, 0, &mut hit as *mut _ as isize))
        };
        if node.0 == 0 { None } else { self.location_of(node) }
    }

    /// Returns the folder to open when the user chose one.
    ///
    /// # Safety
    /// `nm` must be the NMHDR of a WM_NOTIFY from this tree.
    pub unsafe fn on_notify(&self, nm: *const NMHDR) -> Option<Location> {
        match unsafe { (*nm).code } {
            TVN_ITEMEXPANDINGW => {
                let tv = unsafe { &*(nm as *const NMTREEVIEWW) };
                if tv.action.0 & TVE_EXPAND.0 != 0 {
                    self.start_reading(tv.itemNew.hItem);
                }
                None
            }
            TVN_SELCHANGEDW => {
                let tv = unsafe { &*(nm as *const NMTREEVIEWW) };
                if tv.action.0 == TVC_BYMOUSE.0 || tv.action.0 == TVC_BYKEYBOARD.0 {
                    self.location_of(tv.itemNew.hItem)
                } else {
                    None
                }
            }
            NM_CLICK => {
                // Clicking the node that is already selected sends no change, so open it here.
                let mut hit = TVHITTESTINFO::default();
                unsafe {
                    let _ = GetCursorPos(&mut hit.pt);
                    let _ = ScreenToClient(self.hwnd, &mut hit.pt);
                }
                let node = send(self.hwnd, TVM_HITTEST, 0, &mut hit as *mut _ as isize);
                let selected = send(self.hwnd, TVM_GETNEXTITEM, TVGN_CARET as usize, 0);
                if node != 0 && node == selected && hit.flags.0 & TVHT_ONITEM.0 != 0 {
                    self.location_of(HTREEITEM(node))
                } else {
                    None
                }
            }
            _ => None,
        }
    }
}
