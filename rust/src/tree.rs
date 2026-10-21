//! The folder tree. Subfolders are read on a worker thread when a node expands: a dead drive never freezes it.

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::{OsStr, OsString};
use std::os::windows::ffi::OsStrExt;

use windows::Win32::Foundation::{HWND, POINT};
use windows::Win32::Graphics::Gdi::ScreenToClient;
use windows::Win32::Storage::FileSystem::GetDriveTypeW;
use windows::Win32::System::SystemInformation::GetTickCount64;
use windows::Win32::System::WindowsProgramming::DRIVE_REMOTE;
use windows::Win32::UI::Controls::{
    HTREEITEM, NM_CLICK, NMHDR, NMTREEVIEWW, TVE_COLLAPSE, TVE_COLLAPSERESET, TVE_EXPAND, TVGN_CARET,
    TVGN_CHILD, TVGN_DROPHILITE, TVGN_NEXT, TVGN_NEXTVISIBLE, TVGN_PARENT, TVGN_ROOT, TVHITTESTINFO, TVHT_ONITEM, TVHT_ONITEMRIGHT,
    TVI_FIRST, TVI_LAST, TVI_ROOT,
    TVIF_CHILDREN, TVIF_IMAGE, TVIF_SELECTEDIMAGE, TVIF_TEXT, TVINSERTSTRUCTW, TVINSERTSTRUCTW_0, TVITEMEXW,
    TVITEMEXW_CHILDREN, TVITEMW, TVM_DELETEITEM, TVM_EXPAND, TVM_GETITEMW, TVM_GETNEXTITEM, TVM_HITTEST,
    TVM_INSERTITEMW, TVM_SELECTITEM, TVM_SETEXTENDEDSTYLE, TVM_SETIMAGELIST, TVM_SETITEMW, TVN_DELETEITEMW, TVN_ITEMEXPANDEDW,
    TVN_ITEMEXPANDINGW, TVN_SELCHANGEDW, TVS_EX_DOUBLEBUFFER, TVS_EX_FADEINOUTEXPANDOS, TVS_FULLROWSELECT,
    TVS_HASBUTTONS, TVS_LINESATROOT, TVS_NOHSCROLL, TVS_SHOWSELALWAYS, TVS_TRACKSELECT, TVSIL_NORMAL, TVC_BYKEYBOARD,
    TVC_BYMOUSE,
};
use windows::Win32::UI::Shell::{
    FOLDERID_Desktop, FOLDERID_Documents, FOLDERID_Downloads, FOLDERID_Music, FOLDERID_Pictures, FOLDERID_Videos,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, GetCursorPos, HMENU, WINDOW_EX_STYLE, WINDOW_STYLE, WS_CHILD, WS_TABSTOP, WS_VISIBLE,
};
use windows::core::{PWSTR, w};

use crate::folder::{Location, SortKey, drive_roots, is_directory, is_shortcut, list_folder, sort_items, to_lower};
use crate::shell::{
    IconBatch, describe, describe_shortcut, folder_icon, in_background, known_folder, post, read_own_icons, set_theme,
    shortcut_target, system_image_list,
};
use crate::util::{pcwstr, send, wide};

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
    bookmarks_empty: Cell<bool>,              // Bookmarks was read and found empty (it keeps its arrow)
    open_at: Cell<u64>,                       // when the node a drag lights up opens (0: done)
    drop_folder: RefCell<Option<Location>>,   // and where a drop on that node goes
}

// ---------------------------------------------------------------- reading, on a worker thread

/// Subfolders of a location (This PC: the drives), and its shortcuts if asked, into `batch`. Worker thread: may block.
fn read_children(batch: &mut TreeBatch, location: &Location, shortcuts: bool) {
    match location {
        Location::ThisPc => {
            // The drives (not via list_folder: its free-space check can wait on a network drive).
            batch.children = drive_roots()
                .into_iter()
                .map(|root| {
                    let drive = Location::dir(root);
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

/// The local folder a node stands for: its own, or a bookmark shortcut's target (read from the .lnk alone). None for
/// This PC, a link to a file or a web page, and anything on the network: asking Windows about a server that is off
/// would freeze the drag.
fn local_folder(location: Location) -> Option<Location> {
    let path = location.path()?.to_path_buf(); // This PC: none
    let location = if is_shortcut(path.as_os_str()) {
        match shortcut_target(&path)? {
            (target, true) => target,
            _ => return None, // a link to a file
        }
    } else {
        location
    };
    let w: Vec<u16> = location.path()?.as_os_str().encode_wide().collect();
    let local = w.len() >= 3
        && w[1] == u16::from(b':')
        && unsafe { GetDriveTypeW(pcwstr(&[w[0], w[1], w[2], 0])) } != DRIVE_REMOTE;
    local.then_some(location)
}

/// The user folders that exist, as roots, into `batch`. Worker thread: one on a dead server can take 20-60 s.
fn read_user_folders(batch: &mut TreeBatch) {
    for id in [&FOLDERID_Desktop, &FOLDERID_Downloads, &FOLDERID_Documents, &FOLDERID_Pictures, &FOLDERID_Music, &FOLDERID_Videos] {
        if let Some(path) = known_folder(id).filter(|p| is_directory(p)) {
            let location = Location::dir(path);
            let (text, icon) = describe(&location);
            batch.children.push(Child { location, text, icon: Some(icon), shortcut: false });
        }
    }
}

impl FolderTree {
    // ------------------------------------------------------------ the tree

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
        set_theme(hwnd, "Explorer");
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
            bookmarks_empty: Cell::new(false),
            open_at: Cell::new(0),
            drop_folder: RefCell::new(None),
        }
    }

    pub fn hwnd(&self) -> HWND {
        self.hwnd
    }

    pub fn bookmarks(&self) -> &Location {
        &self.bookmarks
    }

    /// Rebuilds the roots: Bookmarks, the user folders, then This PC with its drives.
    pub fn fill(&self) {
        // Forgets each node's path and its read in flight too (TVN_DELETEITEM).
        send(self.hwnd, TVM_DELETEITEM, 0, TVI_ROOT.0);
        let bookmarks = self.add(TVI_ROOT, self.bookmarks.clone(), OsStr::new("Bookmarks"), folder_icon(), false, TVI_LAST);
        self.start_worker(0, read_user_folders); // the user folders arrive later, like the drives
        let (_, icon) = describe(&Location::ThisPc);
        let this_pc = self.add(TVI_ROOT, Location::ThisPc, OsStr::new("This PC"), icon, false, TVI_LAST);
        send(self.hwnd, TVM_EXPAND, TVE_EXPAND.0 as usize, bookmarks.0);
        send(self.hwnd, TVM_EXPAND, TVE_EXPAND.0 as usize, this_pc.0);
    }

    /// This PC, the last root (0 before the tree is filled).
    pub fn last_root(&self) -> isize {
        let mut last = self.root();
        while last != 0 {
            match send(self.hwnd, TVM_GETNEXTITEM, TVGN_NEXT as usize, last) {
                0 => break,
                next => last = next,
            }
        }
        last
    }

    /// Drives came or went: re-read This PC only, so expanded folders stay open (4.5); its read in flight is dropped.
    pub fn refresh_drives(&self) {
        let pc = self.last_root();
        if pc == 0 {
            return;
        }
        if send(self.hwnd, TVM_GETNEXTITEM, TVGN_CHILD as usize, pc) == 0 {
            // No drives shown yet: their first read is still in flight (read again, so the drive that came or went is
            // in the list), or This PC is closed and unread (its next expand reads them).
            if self.reading.borrow_mut().remove(&pc).is_some() {
                self.start_reading(HTREEITEM(pc));
            }
            return;
        }
        self.reading.borrow_mut().remove(&pc);
        // Forget the drives; the next expand reads them.
        send(self.hwnd, TVM_EXPAND, (TVE_COLLAPSE.0 | TVE_COLLAPSERESET.0) as usize, pc);
        send(self.hwnd, TVM_EXPAND, TVE_EXPAND.0 as usize, pc); // and read them again
    }

    /// F5: each folder whose subfolders are shown is read again, and each known to have none (it may have some now).
    pub fn refresh(&self) {
        let mut node = self.root();
        while node != 0 {
            self.reread(node);
            node = send(self.hwnd, TVM_GETNEXTITEM, TVGN_NEXTVISIBLE as usize, node);
        }
    }

    /// The same for one folder's nodes: the folder shown in the list when its subfolders changed, or the parent of a
    /// node whose own menu was used (the node's folder may be gone).
    pub fn refresh_folder(&self, dir: &Location) {
        let nodes: Vec<isize> = self.paths.borrow().iter().filter(|(_, l)| l.same(dir)).map(|(&n, _)| n).collect();
        for node in nodes {
            self.reread(node);
        }
    }

    fn reread(&self, node: isize) {
        let mut item = TVITEMW { mask: TVIF_CHILDREN, hItem: HTREEITEM(node), ..Default::default() };
        send(self.hwnd, TVM_GETITEMW, 0, &mut item as *mut _ as isize);
        let none = item.cChildren.0 == 0 && !is_shortcut(&self.location_of(HTREEITEM(node)).text());
        // An empty Bookmarks keeps its arrow (add_children), so the arrow can't tell: its last read does.
        let empty_root = self.bookmarks_empty.get() && node == self.root();
        if send(self.hwnd, TVM_GETNEXTITEM, TVGN_CHILD as usize, node) != 0 || none || empty_root {
            self.start_reading(HTREEITEM(node));
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
        if self.reading.borrow().contains_key(&node.0) {
            return; // being read
        }
        let location = self.location_of(node);
        let mut top = node.0; // shortcuts show under Bookmarks, the first root, at any depth (5.6)
        loop {
            match send(self.hwnd, TVM_GETNEXTITEM, TVGN_PARENT as usize, top) {
                0 => break,
                up => top = up,
            }
        }
        let shortcuts = top == self.root();
        self.start_worker(node.0, move |batch| read_children(batch, &location, shortcuts));
    }

    /// Runs `reader` on a worker thread to fill a batch, then posts it as the children of `node`.
    fn start_worker(&self, node: isize, reader: impl FnOnce(&mut TreeBatch) + Send + 'static) {
        let read = self.reads.get() + 1;
        self.reads.set(read);
        self.reading.borrow_mut().insert(node, read);
        let (parent, msg, icon_msg) = (self.parent.0 as usize, self.batch_msg, self.icon_msg);
        in_background(move || {
            let mut batch = Box::new(TreeBatch { read, node, failed: false, children: Vec::new() });
            reader(&mut batch);
            let shortcuts: Vec<Location> = batch.children.iter().filter(|c| c.shortcut).map(|c| c.location.clone()).collect();
            post(parent, msg, 0, batch);
            // Then each shortcut's own icon, which asks the target (maybe a server that is off), as the list does.
            if let Some(first) = shortcuts.first() {
                let names = shortcuts.iter().filter_map(|l| l.path()?.file_name().map(OsStr::to_os_string)).collect();
                post(parent, icon_msg, node as usize, Box::new(read_own_icons(&first.parent(), names)));
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
        if batch.failed {
            return; // keep what is shown, and the arrow: expanding again tries again
        }
        if batch.node == self.root() {
            self.bookmarks_empty.set(batch.children.is_empty()); // reread() reads it again
        }
        // Read again (refresh): a folder whose subfolders are the same stays as it is. Otherwise they are put in afresh,
        // and what was open below closes (an open list changed item by item is slow: Windows repaints it for each).
        let mut shown = Vec::new();
        let mut c = send(self.hwnd, TVM_GETNEXTITEM, TVGN_CHILD as usize, node.0);
        while c != 0 {
            shown.push(self.location_of(HTREEITEM(c)).text());
            c = send(self.hwnd, TVM_GETNEXTITEM, TVGN_NEXT as usize, c);
        }
        if !shown.is_empty() {
            if shown.into_iter().eq(batch.children.iter().map(|c| c.location.text())) {
                return;
            }
            send(self.hwnd, TVM_EXPAND, (TVE_COLLAPSE.0 | TVE_COLLAPSERESET.0) as usize, node.0);
        }
        if batch.children.is_empty() {
            // Keep the arrow on Bookmarks (filled later, 5.6); others have no subfolders.
            if batch.node != self.root() {
                let item = TVITEMW { mask: TVIF_CHILDREN, hItem: node, cChildren: TVITEMEXW_CHILDREN(0), ..Default::default() };
                send(self.hwnd, TVM_SETITEMW, 0, &item as *const _ as isize);
            }
            return;
        }
        // Each at the front, the last one first: the same order, but Windows never walks the list to its end for each
        // one, which froze the window for seconds on a folder with 20,000 subfolders.
        for c in batch.children.into_iter().rev() {
            self.add(node, c.location, &c.text, c.icon.unwrap_or_else(folder_icon), c.shortcut, TVI_FIRST);
        }
        send(self.hwnd, TVM_EXPAND, TVE_EXPAND.0 as usize, node.0);
    }

    /// The shortcuts' own icons, read after their node was shown (start_worker). The node may be gone by now.
    pub fn set_icons(&self, node: isize, batch: &IconBatch) {
        if !self.paths.borrow().contains_key(&node) {
            return;
        }
        // By lowercase path: one look-up per node, not one per pair.
        let icons: HashMap<Vec<u16>, i32> = batch
            .icons
            .iter()
            .map(|(name, icon)| (to_lower(Location::dir(batch.dir.join(name)).text().as_os_str()), *icon))
            .collect();
        let mut child = send(self.hwnd, TVM_GETNEXTITEM, TVGN_CHILD as usize, node);
        while child != 0 {
            if let Some(&icon) = icons.get(&to_lower(&self.location_of(HTREEITEM(child)).text())) {
                let item = TVITEMW {
                    mask: TVIF_IMAGE | TVIF_SELECTEDIMAGE,
                    hItem: HTREEITEM(child),
                    iImage: icon,
                    iSelectedImage: icon,
                    ..Default::default()
                };
                send(self.hwnd, TVM_SETITEMW, 0, &item as *const _ as isize);
            }
            child = send(self.hwnd, TVM_GETNEXTITEM, TVGN_NEXT as usize, child);
        }
    }

    // ------------------------------------------------------------ the mouse: paths and drops

    /// The node at a point on the screen (0: none), and on which part of it the point is (TVHT_*).
    fn node_at(&self, screen_pt: POINT) -> (isize, u32) {
        let mut hit = TVHITTESTINFO { pt: screen_pt, ..Default::default() };
        unsafe {
            let _ = ScreenToClient(self.hwnd, &mut hit.pt);
        }
        (send(self.hwnd, TVM_HITTEST, 0, &mut hit as *mut _ as isize), hit.flags.0)
    }

    /// The location under a screen point, or of the selected node for (-1, -1).
    pub fn location_at(&self, pt: POINT) -> Option<Location> {
        let node = if pt.x == -1 && pt.y == -1 {
            send(self.hwnd, TVM_GETNEXTITEM, TVGN_CARET as usize, 0)
        } else {
            self.node_at(pt).0
        };
        (node != 0).then(|| self.location_of(HTREEITEM(node)))
    }

    /// During a drag: the local folder under the cursor, or a bookmark shortcut's, that may take the drop (`None` for
    /// `pt`: the drag left or ended). The node lights up, and a closed one opens after 0.7 s (Windows asks again many
    /// times a second during a drag), so the folders inside it can be reached. Where a drop there goes is worked out
    /// once, as it lights up.
    pub fn drop_target(&self, pt: Option<POINT>) -> Option<Location> {
        let node = pt.map_or(0, |pt| self.node_at(pt).0);
        let now = unsafe { GetTickCount64() };
        if node != send(self.hwnd, TVM_GETNEXTITEM, TVGN_DROPHILITE as usize, 0) {
            send(self.hwnd, TVM_SELECTITEM, TVGN_DROPHILITE as usize, node);
            self.open_at.set(now + 700);
            let folder = if node == 0 { None } else { local_folder(self.location_of(HTREEITEM(node))) };
            *self.drop_folder.borrow_mut() = folder;
        } else if node != 0 && self.open_at.get() != 0 && now >= self.open_at.get() {
            send(self.hwnd, TVM_EXPAND, TVE_EXPAND.0 as usize, node);
            self.open_at.set(0); // once: a folder that can't be read would be read again and again
        }
        self.drop_folder.borrow().clone()
    }

    // ------------------------------------------------------------ notifications

    /// Returns the folder to open when the user chose one.
    ///
    /// # Safety
    /// `nm` must be the NMHDR of a WM_NOTIFY from this tree.
    pub unsafe fn on_notify(&self, nm: *const NMHDR) -> Option<Location> {
        match unsafe { (*nm).code } {
            TVN_ITEMEXPANDINGW => {
                let tv = unsafe { &*(nm as *const NMTREEVIEWW) };
                let node = tv.itemNew.hItem;
                // Not a node whose subfolders are shown: add_children expands the node it has just filled.
                if tv.action.0 & TVE_EXPAND.0 != 0
                    && send(self.hwnd, TVM_GETNEXTITEM, TVGN_CHILD as usize, node.0) == 0
                {
                    self.start_reading(node);
                }
                None
            }
            TVN_ITEMEXPANDEDW => {
                // Forget the subfolders of a collapsed node: the next expand reads them fresh. A read of them still in
                // flight (F5, a drag held over it) is dropped too: its result would open the node again (4.5).
                let tv = unsafe { &*(nm as *const NMTREEVIEWW) };
                if tv.action.0 & TVE_COLLAPSE.0 != 0 {
                    self.reading.borrow_mut().remove(&tv.itemNew.hItem.0);
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
                let mut pt = POINT::default();
                unsafe {
                    let _ = GetCursorPos(&mut pt);
                }
                let (node, part) = self.node_at(pt);
                let selected = send(self.hwnd, TVM_GETNEXTITEM, TVGN_CARET as usize, 0);
                let on_item = part & (TVHT_ONITEM.0 | TVHT_ONITEMRIGHT.0) != 0;
                (node != 0 && node == selected && on_item).then(|| self.location_of(HTREEITEM(node)))
            }
            _ => None,
        }
    }
}
