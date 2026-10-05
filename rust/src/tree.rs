//! The folder tree. Subfolders are read on a worker thread when a node expands: a dead drive never freezes it.

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::{OsStr, OsString};

use windows::Win32::Foundation::{HWND, LPARAM, POINT, WPARAM};
use windows::Win32::Graphics::Gdi::ScreenToClient;
use windows::Win32::System::Com::{COINIT_APARTMENTTHREADED, COINIT_DISABLE_OLE1DDE, CoInitializeEx, CoUninitialize};
use windows::Win32::UI::Controls::{
    HTREEITEM, NM_CLICK, NMHDR, NMTREEVIEWW, SetWindowTheme, TVE_COLLAPSE, TVE_COLLAPSERESET, TVE_EXPAND, TVGN_CARET,
    TVGN_CHILD, TVGN_NEXT, TVGN_ROOT, TVHITTESTINFO, TVHT_ONITEM, TVHT_ONITEMRIGHT, TVI_LAST, TVI_ROOT, TVIF_CHILDREN,
    TVIF_IMAGE, TVIF_SELECTEDIMAGE, TVIF_TEXT, TVINSERTSTRUCTW, TVINSERTSTRUCTW_0, TVIS_EXPANDED, TVITEMEXW,
    TVITEMEXW_CHILDREN, TVITEMW, TVM_DELETEITEM, TVM_EXPAND, TVM_GETITEMSTATE, TVM_GETNEXTITEM, TVM_HITTEST,
    TVM_INSERTITEMW, TVM_SETEXTENDEDSTYLE, TVM_SETIMAGELIST, TVM_SETITEMW, TVN_DELETEITEMW, TVN_ITEMEXPANDEDW,
    TVN_ITEMEXPANDINGW, TVN_SELCHANGEDW, TVS_EX_DOUBLEBUFFER, TVS_EX_FADEINOUTEXPANDOS, TVS_FULLROWSELECT,
    TVS_HASBUTTONS, TVS_LINESATROOT, TVS_NOHSCROLL, TVS_SHOWSELALWAYS, TVS_TRACKSELECT, TVSIL_NORMAL, TVC_BYKEYBOARD,
    TVC_BYMOUSE,
};
use windows::Win32::UI::Shell::{
    FOLDERID_Desktop, FOLDERID_Documents, FOLDERID_Downloads, FOLDERID_Music, FOLDERID_Pictures, FOLDERID_Videos,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, GetCursorPos, HMENU, PostMessageW, WINDOW_EX_STYLE, WINDOW_STYLE, WS_CHILD, WS_TABSTOP, WS_VISIBLE,
};
use windows::core::{PWSTR, w};

use crate::folder::{Location, SortKey, is_shortcut, list_folder, sort_items};
use crate::shell::{IconBatch, describe, describe_shortcut, folder_icon, known_folder, read_own_icons, system_image_list};
use crate::util::{send, wide};

/// Subfolders of one node, read on a worker thread and posted to the tree's parent window.
pub struct TreeBatch {
    read: u32,    // the read this answers; a node deleted and re-read since then has a newer one
    node: isize,  // 0: the user folders, which are roots between Bookmarks and This PC
    failed: bool, // the folder could not be read, so nothing is known about its children
    children: Vec<Child>,
}

struct Child {
    location: Location,
    text: OsString,
    icon: Option<i32>, // None: plain folder icon
    shortcut: bool,
}

pub struct FolderTree {
    hwnd: HWND,
    parent: HWND,
    bookmarks: Location,
    batch_msg: u32,
    icon_msg: u32,
    reads: Cell<u32>,                         // numbers every read started
    paths: RefCell<HashMap<isize, Location>>, // each node's folder (or shortcut); a node leaves it when deleted
    reading: RefCell<HashMap<isize, u32>>,    // nodes whose subfolders are being read, by read number
}

/// Subfolders of a location (This PC: the drives), and its shortcuts if asked, into `batch`. Worker thread: may block.
fn read_children(batch: &mut TreeBatch, location: &Location, shortcuts: bool) {
    match location {
        Location::ThisPc => {
            // The drives (not via list_folder: its free-space check can wait on a network drive).
            let mask = unsafe { windows::Win32::Storage::FileSystem::GetLogicalDrives() };
            batch.children = (0..26u8)
                .filter(|i| mask & (1 << i) != 0)
                .map(|i| {
                    let drive = Location::dir(format!("{}:\\", (b'A' + i) as char));
                    let (text, icon) = describe(&drive);
                    Child { location: drive, text, icon: Some(icon), shortcut: false }
                })
                .collect();
        }
        Location::Dir(_) => {
            let listing = list_folder(location, false, None);
            batch.failed = listing.error.is_some();
            let mut items: Vec<_> =
                listing.items.into_iter().filter(|i| i.is_dir() || (shortcuts && is_shortcut(&i.name))).collect();
            sort_items(&mut items, SortKey::Name, true); // folders first, in natural order
            batch.children = items
                .into_iter()
                .map(|it| {
                    let child = Location::dir(location.join(&it.name));
                    if it.is_dir() {
                        Child { location: child, text: it.name, icon: None, shortcut: false }
                    } else {
                        // Its own icon follows (start_worker).
                        let (text, icon) = describe_shortcut(&location.join(&it.name));
                        Child { location: child, text, icon: Some(icon), shortcut: true }
                    }
                })
                .collect();
        }
    }
}

/// The user folders that exist, as roots, into `batch`. Worker thread: one on a dead server can take 20-60 s.
fn read_user_folders(batch: &mut TreeBatch) {
    for id in [&FOLDERID_Desktop, &FOLDERID_Downloads, &FOLDERID_Documents, &FOLDERID_Pictures, &FOLDERID_Music, &FOLDERID_Videos] {
        if let Some(path) = known_folder(id).filter(|p| p.is_dir()) {
            let location = Location::dir(path);
            let (text, icon) = describe(&location);
            batch.children.push(Child { location, text, icon: Some(icon), shortcut: false });
        }
    }
}

impl FolderTree {
    /// Posts `batch_msg` (a `Box<TreeBatch>`, for `add_children`) and `icon_msg` (a `Box<IconBatch>`, for
    /// `set_icons`) to `parent`. `bookmarks` is the first root, open, with its shortcuts as well as its folders.
    pub fn create(parent: HWND, id: i32, batch_msg: u32, icon_msg: u32, bookmarks: Location) -> FolderTree {
        // TVS_LINESATROOT gives the roots an expand arrow; without TVS_HASLINES no lines show, as in Explorer.
        let style = WS_CHILD.0 | WS_VISIBLE.0 | WS_TABSTOP.0 | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS
            | TVS_TRACKSELECT | TVS_FULLROWSELECT | TVS_NOHSCROLL;
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
        send(hwnd, TVM_SETIMAGELIST, TVSIL_NORMAL as usize, system_image_list().0);
        FolderTree {
            hwnd,
            parent,
            bookmarks,
            batch_msg,
            icon_msg,
            reads: Cell::new(0),
            paths: RefCell::new(HashMap::new()),
            reading: RefCell::new(HashMap::new()),
        }
    }

    pub fn hwnd(&self) -> HWND {
        self.hwnd
    }

    /// Rebuilds the roots: Bookmarks, the user folders, then This PC with its drives.
    pub fn fill(&self) {
        self.reading.borrow_mut().clear(); // results still being read for the old nodes will be dropped
        send(self.hwnd, TVM_DELETEITEM, 0, TVI_ROOT.0); // forgets each node's path too (TVN_DELETEITEM)
        let bookmarks = self.add(TVI_ROOT, self.bookmarks.clone(), OsStr::new("Bookmarks"), folder_icon(), false, TVI_LAST);
        self.start_worker(0, read_user_folders); // the user folders arrive later, like the drives
        let (_, icon) = describe(&Location::ThisPc);
        let this_pc = self.add(TVI_ROOT, Location::ThisPc, OsStr::new("This PC"), icon, false, TVI_LAST);
        send(self.hwnd, TVM_EXPAND, TVE_EXPAND.0 as usize, bookmarks.0);
        send(self.hwnd, TVM_EXPAND, TVE_EXPAND.0 as usize, this_pc.0);
    }

    /// Drives came or went: re-read This PC only, so expanded folders stay open (4.5); its read in flight is dropped.
    pub fn refresh_drives(&self) {
        let mut pc = self.root();
        if pc == 0 {
            return;
        }
        loop {
            let next = send(self.hwnd, TVM_GETNEXTITEM, TVGN_NEXT as usize, pc);
            if next == 0 {
                break; // This PC is the last root
            }
            pc = next;
        }
        let expanded = send(self.hwnd, TVM_GETITEMSTATE, pc as usize, TVIS_EXPANDED.0 as isize) & TVIS_EXPANDED.0 as isize != 0;
        self.reading.borrow_mut().remove(&pc);
        // Forget the drives; the next expand reads them.
        send(self.hwnd, TVM_EXPAND, (TVE_COLLAPSE.0 | TVE_COLLAPSERESET.0) as usize, pc);
        if expanded {
            send(self.hwnd, TVM_EXPAND, TVE_EXPAND.0 as usize, pc);
        }
    }

    fn add(&self, parent: HTREEITEM, location: Location, text: &OsStr, icon: i32, leaf: bool, after: HTREEITEM) -> HTREEITEM {
        let text = wide(text);
        let ins = TVINSERTSTRUCTW {
            hParent: parent,
            hInsertAfter: after,
            Anonymous: TVINSERTSTRUCTW_0 {
                itemex: TVITEMEXW {
                    mask: TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN,
                    pszText: PWSTR(text.as_ptr() as *mut u16),
                    iImage: icon,
                    iSelectedImage: icon,
                    cChildren: TVITEMEXW_CHILDREN(if leaf { 0 } else { 1 }), // a folder's are unknown until expanded
                    ..Default::default()
                },
            },
        };
        let node = send(self.hwnd, TVM_INSERTITEMW, 0, &ins as *const _ as isize);
        if node != 0 {
            self.paths.borrow_mut().insert(node, location);
        }
        HTREEITEM(node)
    }

    fn root(&self) -> isize {
        send(self.hwnd, TVM_GETNEXTITEM, TVGN_ROOT as usize, 0)
    }

    fn location_of(&self, node: HTREEITEM) -> Location {
        self.paths.borrow()[&node.0].clone() // a node that is not there is a dead handle: stop loudly (4.10)
    }

    fn start_reading(&self, node: HTREEITEM) {
        if send(self.hwnd, TVM_GETNEXTITEM, TVGN_CHILD as usize, node.0) != 0 || self.reading.borrow().contains_key(&node.0) {
            return; // already read, or being read
        }
        let location = self.location_of(node);
        let shortcuts = node.0 == self.root(); // the Bookmarks node
        self.start_worker(node.0, move |batch| read_children(batch, &location, shortcuts));
    }

    /// Runs `reader` on a worker thread to fill a batch, then posts it as the children of `node`.
    fn start_worker(&self, node: isize, reader: impl FnOnce(&mut TreeBatch) + Send + 'static) {
        let read = self.reads.get() + 1;
        self.reads.set(read);
        self.reading.borrow_mut().insert(node, read);
        let (parent, msg, icon_msg) = (self.parent.0 as usize, self.batch_msg, self.icon_msg);
        std::thread::spawn(move || unsafe {
            let co = CoInitializeEx(None, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            let mut batch = Box::new(TreeBatch { read, node, failed: false, children: Vec::new() });
            reader(&mut batch);
            let shortcuts: Vec<Location> = batch.children.iter().filter(|c| c.shortcut).map(|c| c.location.clone()).collect();
            let batch = Box::into_raw(batch);
            if PostMessageW(Some(HWND(parent as _)), msg, WPARAM(0), LPARAM(batch as isize)).is_err() {
                drop(Box::from_raw(batch)); // the window is gone
            }
            // Then each shortcut's own icon, which asks the target (maybe a server that is off), as the list does.
            if let Some(first) = shortcuts.first() {
                let names = shortcuts.iter().filter_map(|l| l.path()?.file_name().map(OsStr::to_os_string)).collect();
                let icons = Box::into_raw(Box::new(read_own_icons(&first.parent(), names)));
                if PostMessageW(Some(HWND(parent as _)), icon_msg, WPARAM(0), LPARAM(icons as isize)).is_err() {
                    drop(Box::from_raw(icons));
                }
            }
            if co.is_ok() {
                CoUninitialize();
            }
        });
    }

    pub fn add_children(&self, batch: TreeBatch) {
        // Nodes in `reading` are alive (deleted ones leave in TVN_DELETEITEM); the read number spots a reused handle.
        if self.reading.borrow().get(&batch.node) != Some(&batch.read) {
            return;
        }
        self.reading.borrow_mut().remove(&batch.node);
        if batch.node == 0 {
            // The user folders go after Bookmarks (always the first root) and before This PC.
            let mut after = HTREEITEM(self.root());
            for c in batch.children {
                after = self.add(TVI_ROOT, c.location, &c.text, c.icon.unwrap_or_else(folder_icon), false, after);
            }
            return;
        }
        let node = HTREEITEM(batch.node);
        if batch.children.is_empty() {
            // Keep the arrow when the read failed, and on Bookmarks (filled later, 5.6); others have no subfolders.
            if !batch.failed && batch.node != self.root() {
                let item = TVITEMW { mask: TVIF_CHILDREN, hItem: node, cChildren: TVITEMEXW_CHILDREN(0), ..Default::default() };
                send(self.hwnd, TVM_SETITEMW, 0, &item as *const _ as isize);
            }
            return;
        }
        for c in batch.children {
            self.add(node, c.location, &c.text, c.icon.unwrap_or_else(folder_icon), c.shortcut, TVI_LAST);
        }
        send(self.hwnd, TVM_EXPAND, TVE_EXPAND.0 as usize, node.0);
    }

    /// The shortcuts' own icons, read after their node was shown (start_worker).
    pub fn set_icons(&self, batch: &IconBatch) {
        if !batch.dir.same(&self.bookmarks) {
            return; // shortcuts are shown only under Bookmarks
        }
        let mut child = send(self.hwnd, TVM_GETNEXTITEM, TVGN_CHILD as usize, self.root());
        while child != 0 {
            let location = self.location_of(HTREEITEM(child));
            for (name, icon) in &batch.icons {
                if location.same(&Location::dir(batch.dir.join(name))) {
                    let item = TVITEMW {
                        mask: TVIF_IMAGE | TVIF_SELECTEDIMAGE,
                        hItem: HTREEITEM(child),
                        iImage: *icon,
                        iSelectedImage: *icon,
                        ..Default::default()
                    };
                    send(self.hwnd, TVM_SETITEMW, 0, &item as *const _ as isize);
                }
            }
            child = send(self.hwnd, TVM_GETNEXTITEM, TVGN_NEXT as usize, child);
        }
    }

    /// The location under a screen point, or of the selected node for (-1, -1).
    pub fn location_at(&self, pt: POINT) -> Option<Location> {
        let node = if pt.x == -1 && pt.y == -1 {
            send(self.hwnd, TVM_GETNEXTITEM, TVGN_CARET as usize, 0)
        } else {
            let mut hit = TVHITTESTINFO { pt, ..Default::default() };
            unsafe {
                let _ = ScreenToClient(self.hwnd, &mut hit.pt);
            }
            send(self.hwnd, TVM_HITTEST, 0, &mut hit as *mut _ as isize)
        };
        (node != 0).then(|| self.location_of(HTREEITEM(node)))
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
            TVN_ITEMEXPANDEDW => {
                // Forget the subfolders of a collapsed node: the next expand reads them fresh.
                let tv = unsafe { &*(nm as *const NMTREEVIEWW) };
                if tv.action.0 & TVE_COLLAPSE.0 != 0 {
                    send(self.hwnd, TVM_EXPAND, (TVE_COLLAPSE.0 | TVE_COLLAPSERESET.0) as usize, tv.itemNew.hItem.0);
                }
                None
            }
            TVN_DELETEITEMW => {
                // The handle dies with the node (collapse, fill(), shutdown); a read still in flight must not insert under it.
                let node = unsafe { (*(nm as *const NMTREEVIEWW)).itemOld.hItem.0 };
                self.reading.borrow_mut().remove(&node);
                self.paths.borrow_mut().remove(&node);
                None
            }
            TVN_SELCHANGEDW => {
                let tv = unsafe { &*(nm as *const NMTREEVIEWW) };
                let by_user = tv.action.0 == TVC_BYMOUSE.0 || tv.action.0 == TVC_BYKEYBOARD.0;
                by_user.then(|| self.location_of(tv.itemNew.hItem))
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
                let on_item = hit.flags.0 & (TVHT_ONITEM.0 | TVHT_ONITEMRIGHT.0) != 0;
                (node != 0 && node == selected && on_item).then(|| self.location_of(HTREEITEM(node)))
            }
            _ => None,
        }
    }
}
