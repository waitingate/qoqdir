//! `--selftest`, only in a build with the `selftest` feature: a scripted session in the real app. Keys go
//! straight to `App::on_key`, never to other windows. The steps are the C++ version's, in the same order and
//! with the same names.

use std::cell::{Cell, RefCell};
use std::ffi::{OsStr, OsString};
use std::io::Write;
use std::os::windows::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::OnceLock;
use std::time::{Duration, Instant};

use windows::Win32::Foundation::{HGLOBAL, HWND, LPARAM, POINT, POINTL, RECT, WPARAM};
use windows::Win32::Graphics::Gdi::{COLOR_WINDOW, ClientToScreen, GetSysColor, GetSysColorBrush};
use windows::Win32::Storage::FileSystem::{
    FILE_ATTRIBUTE_ARCHIVE, FILE_ATTRIBUTE_HIDDEN, FILE_FLAGS_AND_ATTRIBUTES, GetFileAttributesW, GetLongPathNameW,
    SetFileAttributesW,
};
use windows::Win32::System::Com::IDataObject;
use windows::Win32::System::DataExchange::{CloseClipboard, GetClipboardData, OpenClipboard};
use windows::Win32::System::Diagnostics::Debug::{GetErrorMode, SEM_FAILCRITICALERRORS};
use windows::Win32::System::Memory::{GlobalLock, GlobalUnlock};
use windows::Win32::System::Ole::{DROPEFFECT_COPY, DROPEFFECT_NONE};
use windows::Win32::System::SystemInformation::GetSystemDirectoryW;
use windows::Win32::System::SystemServices::MK_LBUTTON;
use windows::Win32::UI::Controls::{
    EM_GETSEL, EM_REPLACESEL, HTREEITEM, LVIF_IMAGE, LVIR_LABEL, LVITEMW, LVM_CANCELEDITLABEL, LVM_GETBKCOLOR, LVM_GETCOUNTPERPAGE,
    LVM_GETTEXTBKCOLOR, TVM_GETBKCOLOR,
    LVM_GETITEMRECT, LVM_GETITEMW, SB_GETTEXTLENGTHW, SB_GETTEXTW, TCIF_TEXT, TCITEMW,
    TCM_GETCURSEL, TCM_GETITEMCOUNT, TCM_GETITEMRECT, TCM_GETITEMW, TCM_SETCURFOCUS, UPDOWN_CLASSW,
    TVE_EXPAND, TVGN_CARET, TVGN_CHILD, TVGN_DROPHILITE, TVGN_NEXT, TVGN_ROOT, TVHITTESTINFO, TVHT_ONITEMBUTTON, TVIF_CHILDREN,
    TVIF_IMAGE,
    TVIF_TEXT, TVIS_EXPANDED, TVITEMW, TVM_EXPAND, TVM_GETCOUNT, TVM_GETINDENT, TVM_GETITEMRECT, TVM_GETITEMSTATE,
    TVM_GETITEMW, TVM_GETNEXTITEM, TVM_HITTEST, TVM_SELECTITEM,
};
use windows::Win32::UI::Input::KeyboardAndMouse::{
    GetFocus, VIRTUAL_KEY, VK_BACK, VK_DELETE, VK_DOWN, VK_ESCAPE, VK_F2, VK_F5, VK_LEFT, VK_NEXT, VK_PRIOR, VK_RETURN,
    VK_RIGHT, VK_TAB,
};
use windows::Win32::UI::Shell::{
    DragQueryFileW, FOLDERID_ComputerFolder, FOLDERID_Desktop, HDROP, SHFILEINFOW, SHGFI_DISPLAYNAME, SHGFI_SMALLICON,
    SHGFI_SYSICONINDEX, SHGetFileInfoW, SHGetKnownFolderIDList, StrCmpLogicalW,
};
use windows::Win32::UI::WindowsAndMessaging::{
    FindWindowExW, GCLP_HBRBACKGROUND, GWLP_USERDATA, GetClassLongPtrW, GetClientRect, GetWindowLongPtrW, GetWindowRect, GetWindowTextLengthW, IsWindowVisible,
    KillTimer, PostMessageW, SW_SHOWMINNOACTIVE, SW_SHOWNOACTIVATE, SetTimer, ShowWindow, WM_CHAR, WM_CLOSE,
    WM_DEVICECHANGE, WM_DPICHANGED, WM_KEYDOWN, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MBUTTONUP, WM_MOUSEMOVE, WM_MOUSEWHEEL,
    WM_SYSKEYDOWN, WM_TIMER,
};

use crate::app::{App, Key, Nav, TIMER_LOADING, TIMER_REFRESH, ViewState};
use crate::folder::{Item, Listing, Location, SortKey, exists, is_directory, is_shortcut, list_folder, normalize, sort_items};
use crate::shell::{
    copy_to_clipboard, data_object_at, item_menu_at, known_folder, paste_into, shortcut_target, type_name,
};
use crate::util::{Pidl, focus, from_wide, pcwstr, scale, send, set_text, wide, window_text};

type Act = Rc<dyn Fn(&App)>;
type Check = Rc<dyn Fn(&App) -> bool>;

struct Step {
    name: &'static str,
    act: Option<Act>,
    done: Check,
    timeout_ms: u128,
    while_loading: bool, // start without waiting for the current folder load to finish
}

impl Step {
    fn wait(&mut self, ms: u128) -> &mut Step {
        self.timeout_ms = ms;
        self
    }

    fn during_load(&mut self) -> &mut Step {
        self.while_loading = true;
        self
    }
}

#[derive(Default)]
struct Steps(Vec<Step>);

impl Steps {
    /// `act` runs once, then `done` is asked until it says yes, or the step fails after 5 s.
    fn add(&mut self, name: &'static str, act: impl Fn(&App) + 'static, done: impl Fn(&App) -> bool + 'static) -> &mut Step {
        self.push(name, Some(Rc::new(act)), Rc::new(done))
    }

    fn check(&mut self, name: &'static str, done: impl Fn(&App) -> bool + 'static) -> &mut Step {
        self.push(name, None, Rc::new(done))
    }

    /// A step that opens the test folder again.
    fn home(&mut self, name: &'static str) {
        self.add(name, |a| a.open(Location::dir(test_dir()), Nav::New, ViewState::default()), |a| a.at(&test_dir()));
    }

    fn push(&mut self, name: &'static str, act: Option<Act>, done: Check) -> &mut Step {
        self.0.push(Step { name, act, done, timeout_ms: 5000, while_loading: false });
        self.0.last_mut().expect("just added")
    }
}

struct SelfTest {
    steps: Vec<Step>,
    index: usize,
    started: Option<Instant>, // set once the step's act has run
    busy: bool,
    failures: usize,
}

// What steps hand on to their checks, or to later steps.
thread_local! {
    static NOTE: RefCell<String> = const { RefCell::new(String::new()) }; // shown in the log after the step's name
    static T0: Cell<Option<Instant>> = const { Cell::new(None) };
    static KEY_MS: Cell<u128> = const { Cell::new(0) }; // how long a key press kept the window busy
    static ROWS: Cell<usize> = const { Cell::new(0) };  // list rows before a refresh that must wait
    static TOP: Cell<usize> = const { Cell::new(0) };   // a scroll position to come back to
    // Before a scaling change: the dots per inch, the row height, the first column's width and the window's place.
    static BEFORE: Cell<(u32, i32, i32, RECT)> = const { Cell::new((0, 0, 0, RECT { left: 0, top: 0, right: 0, bottom: 0 })) };
    static EFFECT: Cell<u32> = const { Cell::new(0) };  // what a simulated drag was allowed to do
    static DRAGGED: RefCell<Option<IDataObject>> = const { RefCell::new(None) }; // what a simulated drag carries
    static COUNT: Cell<u32> = const { Cell::new(0) };   // a load count to compare with
    static LOADS: Cell<(u32, u32)> = const { Cell::new((0, 0)) };
    static FLAG: Cell<bool> = const { Cell::new(false) };
    static NEXT: RefCell<OsString> = const { RefCell::new(OsString::new()) }; // the item after the one Delete removes
    static RUN: RefCell<Option<SelfTest>> = const { RefCell::new(None) }; // the steps, and how far the run is
    static EXIT_CODE: Cell<i32> = const { Cell::new(1) }; // a run that never finishes (closed by hand) has not passed
}

const TIMER_SELFTEST: usize = 50;

/// The process's exit code: 0 only if every step passed.
pub fn exit_code() -> i32 {
    EXIT_CODE.get()
}

fn note(text: String) {
    NOTE.with(|n| *n.borrow_mut() = text);
}

fn noted() -> String {
    NOTE.with(|n| n.borrow().clone())
}

/// How long the current step has waited since its act.
fn step_ms() -> u128 {
    RUN.with_borrow(|run| run.as_ref().and_then(|t| t.started).map_or(0, |s| s.elapsed().as_millis()))
}

/// %TEMP%\qoqdir-rs-selftest, with long names: paths Windows hands back (shortcut targets) are never RUNNER~1.
pub fn test_dir() -> PathBuf {
    static DIR: OnceLock<PathBuf> = OnceLock::new();
    DIR.get_or_init(|| {
        let temp = std::env::temp_dir();
        let mut long = vec![0u16; 32768];
        let n = unsafe { GetLongPathNameW(pcwstr(&wide(&temp)), Some(&mut long)) } as usize;
        let temp = if n > 0 && n < long.len() { PathBuf::from(from_wide(&long)) } else { temp };
        temp.join(format!("{}-rs-selftest", crate::APP_NAME.to_lowercase()))
    })
    .clone()
}

/// A path in the test folder.
fn p(name: &str) -> PathBuf {
    test_dir().join(name)
}

fn log_path() -> PathBuf {
    let mut path = test_dir().into_os_string();
    path.push(".log");
    path.into()
}

/// One line of the log, written at once (7.2): a hang or a crash later still leaves the steps so far.
fn log(line: &str) {
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(log_path()) {
        let _ = f.write_all(line.as_bytes());
    }
}

/// Makes a fresh test folder and returns it.
pub fn prepare() -> Location {
    let root = test_dir();
    let _ = std::fs::remove_dir_all(&root);
    let _ = std::fs::create_dir_all(root.join("sub"));
    for name in ["file1.txt", "file2.txt", "file10.txt", "\u{5831}\u{544A}.pdf"] {
        write_file(&root.join(name));
    }
    Location::dir(root)
}

fn write_file(path: &Path) {
    let _ = std::fs::write(path, "x");
}

fn clipboard_files() -> Vec<PathBuf> {
    const CF_HDROP: u32 = 15;
    let mut files = Vec::new();
    unsafe {
        if OpenClipboard(None).is_err() {
            return files;
        }
        if let Ok(data) = GetClipboardData(CF_HDROP) {
            let global = HGLOBAL(data.0);
            let drop = HDROP(GlobalLock(global));
            let n = DragQueryFileW(drop, u32::MAX, None);
            for i in 0..n {
                let mut buf = [0u16; 260];
                if DragQueryFileW(drop, i, Some(&mut buf)) > 0 {
                    files.push(PathBuf::from(from_wide(&buf)));
                }
            }
            let _ = GlobalUnlock(global);
        }
        let _ = CloseClipboard();
    }
    files
}

/// Paste only the test's own file: after a failed Ctrl+C, pasting would move files the user cut.
fn holds_test_file() -> bool {
    let files = clipboard_files();
    files.len() == 1 && Location::dir(&files[0]).same(&Location::dir(p("file1.txt")))
}

/// A shortcut to `target`; "" is This PC, which has no path.
fn make_shortcut(lnk: &Path, target: impl AsRef<OsStr>) {
    use windows::Win32::System::Com::{CLSCTX_INPROC_SERVER, CoCreateInstance, IPersistFile};
    use windows::Win32::UI::Shell::{IShellLinkW, ShellLink};
    use windows::core::Interface;
    unsafe {
        let Ok(link) = CoCreateInstance::<_, IShellLinkW>(&ShellLink, None, CLSCTX_INPROC_SERVER) else { return };
        if !target.as_ref().is_empty() {
            let _ = link.SetPath(pcwstr(&wide(target)));
        } else if let Ok(pc) = SHGetKnownFolderIDList(&FOLDERID_ComputerFolder, 0, None) {
            let pc = Pidl(pc);
            let _ = link.SetIDList(pc.0);
        }
        if let Ok(file) = link.cast::<IPersistFile>() {
            let _ = file.Save(pcwstr(&wide(lnk)), true);
        }
    }
}

/// The same path with a backslash at the end, as hand-made shortcuts often have it.
fn with_slash(path: PathBuf) -> OsString {
    let mut s = path.into_os_string();
    s.push("\\");
    s
}

fn letter(c: u8) -> VIRTUAL_KEY {
    VIRTUAL_KEY(c as u16)
}

const CTRL: u8 = 1;
const SHIFT: u8 = 2;
const ALT: u8 = 4;

/// The message a drive change sends: a volume came or went.
fn volume_change(a: &App, event: usize) {
    let volume: [u32; 5] = [20, 2, 0, 0, 0]; // DEV_BROADCAST_VOLUME: size, DBT_DEVTYP_VOLUME, then unused fields
    send(a.wnd, WM_DEVICECHANGE, event, volume.as_ptr() as isize);
}
const DBT_DEVICEARRIVAL: usize = 0x8000;
const DBT_DEVICEREMOVECOMPLETE: usize = 0x8004;

/// A point as mouse messages carry it.
fn packed(x: i32, y: i32) -> isize {
    x as u16 as isize | ((y as u16 as isize) << 16)
}

fn tab_rect(a: &App, index: usize) -> RECT {
    let mut r = RECT::default();
    send(a.tab_bar, TCM_GETITEMRECT, index, &mut r as *mut _ as isize);
    r
}

/// A mouse drag of tab `from` to `x`, as the tab bar gets it.
fn drag_along_bar(a: &App, from: usize, x: i32) {
    let r = tab_rect(a, from);
    let y = (r.top + r.bottom) / 2;
    let button = MK_LBUTTON.0 as usize;
    send(a.tab_bar, WM_LBUTTONDOWN, button, packed((r.left + r.right) / 2, y));
    send(a.tab_bar, WM_MOUSEMOVE, button, packed(x, y));
    send(a.tab_bar, WM_LBUTTONUP, 0, packed(x, y));
}

/// The mouse wheel turned with the cursor over the tab bar.
fn wheel(a: &App, delta: i16) {
    let pt = a.tab_point(0);
    send(a.tab_bar, WM_MOUSEWHEEL, (delta as u16 as usize) << 16, packed(pt.x, pt.y));
}

/// A tab's name as the tab bar shows it.
fn caption(a: &App, index: usize) -> Vec<u16> {
    let mut text = [0u16; 128];
    let mut item = TCITEMW { mask: TCIF_TEXT, pszText: windows::core::PWSTR(text.as_mut_ptr()), cchTextMax: 128, ..Default::default() };
    send(a.tab_bar, TCM_GETITEMW, index, &mut item as *mut _ as isize);
    from_wide(&text).encode_wide().collect()
}

/// The width every tab has (0: they differ).
fn tab_width(a: &App) -> i32 {
    let widths: Vec<i32> = (0..send(a.tab_bar, TCM_GETITEMCOUNT, 0, 0) as usize)
        .map(|i| {
            let r = tab_rect(a, i);
            r.right - r.left
        })
        .collect();
    if widths.windows(2).all(|w| w[0] == w[1]) { widths.last().copied().unwrap_or(0) } else { 0 }
}

/// The tab bar's width.
fn bar_width(a: &App) -> i32 {
    let mut bar = RECT::default();
    unsafe {
        let _ = GetClientRect(a.tab_bar, &mut bar);
    }
    bar.right
}

/// Files that exist only in the list, all selected: a copy that asked the disk about each one would find nothing.
fn show_ghosts(a: &App) {
    let items = (0..3)
        .map(|i| Item { name: format!("ghost {i}.dat").into(), attrs: FILE_ATTRIBUTE_ARCHIVE.0, icon: -1, ..Default::default() })
        .collect();
    a.list.set_listing(Listing { location: Location::dir(p("big")), items, ..Default::default() });
    a.list.select_all();
}

impl App {
    /// A key press, fed to the app as the message loop would, and to the control when the app leaves it.
    fn press(&self, vk: VIRTUAL_KEY, mods: u8) {
        let (ctrl, shift, alt) = (mods & CTRL != 0, mods & SHIFT != 0, mods & ALT != 0);
        let focus = unsafe { GetFocus() };
        let message = if alt { WM_SYSKEYDOWN } else { WM_KEYDOWN };
        if self.on_key(Key { message, code: vk.0 as usize, focus, ctrl, shift, alt }) {
            return;
        }
        send(focus, WM_KEYDOWN, vk.0 as usize, 0);
        // A real Enter, Esc or Backspace also sends a character (Ctrl+Backspace: 127), if the control is still there.
        if unsafe { GetFocus() } == focus && (vk == VK_RETURN || vk == VK_ESCAPE || vk == VK_BACK) {
            send(focus, WM_CHAR, if vk == VK_BACK && ctrl { 127 } else { vk.0 as usize }, 0);
        }
    }

    fn type_text(&self, text: &str) {
        for c in text.encode_utf16() {
            let focus = unsafe { GetFocus() };
            let key = Key { message: WM_CHAR, code: c as usize, focus, ctrl: false, shift: false, alt: false };
            if !self.on_key(key) {
                send(focus, WM_CHAR, c as usize, 0);
            }
        }
    }

    /// Types into the address bar and presses Enter.
    fn go_to(&self, text: impl AsRef<OsStr>) {
        set_text(self.address, text);
        focus(self.address);
        self.press(VK_RETURN, 0);
    }

    fn idle(&self) -> bool {
        !self.is_loading()
    }

    fn tab_location(&self) -> Location {
        let st = self.st.borrow();
        st.tabs[st.active].location.clone()
    }

    fn tab_path(&self, index: usize) -> Location {
        self.st.borrow().tabs.get(index).map_or(Location::ThisPc, |t| t.location.clone())
    }

    fn tabs(&self) -> usize {
        self.st.borrow().tabs.len()
    }

    fn active(&self) -> usize {
        self.st.borrow().active
    }

    fn unavailable(&self) -> bool {
        let st = self.st.borrow();
        st.tabs[st.active].unavailable
    }

    fn at(&self, dir: &Path) -> bool {
        self.idle() && self.tab_location().same(&Location::dir(dir))
    }

    fn at_pc(&self) -> bool {
        self.idle() && matches!(self.tab_location(), Location::ThisPc)
    }

    fn status_message(&self) -> String {
        let len = (send(self.status, SB_GETTEXTLENGTHW, 1, 0) & 0xFFFF) as usize;
        let mut buf = vec![0u16; len + 1];
        send(self.status, SB_GETTEXTW, 1, buf.as_mut_ptr() as isize);
        from_wide(&buf).to_string_lossy().into_owned()
    }

    fn listed(&self, name: &str) -> bool {
        (0..self.list.count()).any(|r| self.list.item(r).is_some_and(|i| i.name == name))
    }

    fn focused_is(&self, name: &str) -> bool {
        self.list.focused_name().is_some_and(|n| n == name)
    }

    fn select_one(&self, name: &str) -> Option<usize> {
        self.list.select(&[name.into()], Some(OsStr::new(name)))
    }

    /// The focused row is on screen: within the page that starts at the top row.
    fn focused_on_screen(&self) -> bool {
        let (row, first) = (self.list.focused(), self.list.top_row());
        let page = send(self.list.hwnd(), LVM_GETCOUNTPERPAGE, 0, 0) as usize;
        note(format!("row {}, top row {first}", row.map_or(-1, |r| r as i64)));
        row.is_some_and(|r| r >= first && r < first + page)
    }

    fn filter_text(&self) -> String {
        window_text(self.filter).to_string_lossy().into_owned()
    }

    fn edit_text(&self) -> String {
        self.list.edit_control().map(|e| window_text(e).to_string_lossy().into_owned()).unwrap_or_default()
    }

    fn cancel_rename(&self) {
        send(self.list.hwnd(), LVM_CANCELEDITLABEL, 0, 0);
    }

    /// The middle of a tab, in screen coordinates, where a drag is aimed.
    fn tab_point(&self, index: usize) -> POINTL {
        let mut r = RECT::default();
        send(self.tab_bar, TCM_GETITEMRECT, index, &mut r as *mut _ as isize);
        let mut pt = POINT { x: (r.left + r.right) / 2, y: (r.top + r.bottom) / 2 };
        unsafe {
            let _ = ClientToScreen(self.tab_bar, &mut pt);
        }
        POINTL { x: pt.x, y: pt.y }
    }

    fn tree_root(&self) -> isize {
        send(self.tree.hwnd(), TVM_GETNEXTITEM, TVGN_ROOT as usize, 0)
    }

    fn tree_child(&self, node: isize) -> isize {
        send(self.tree.hwnd(), TVM_GETNEXTITEM, TVGN_CHILD as usize, node)
    }

    fn tree_next(&self, node: isize) -> isize {
        send(self.tree.hwnd(), TVM_GETNEXTITEM, TVGN_NEXT as usize, node)
    }

    fn tree_last_root(&self) -> isize {
        let mut last = self.tree_root();
        while self.tree_next(last) != 0 {
            last = self.tree_next(last);
        }
        last
    }

    /// Bookmarks' folder "two".
    fn tree_two(&self) -> isize {
        self.tree_next(self.tree_child(self.tree_root()))
    }

    /// The middle of a tree node, in screen coordinates, where a drag is aimed.
    fn node_point(&self, node: isize) -> POINTL {
        let mut r = RECT::default();
        unsafe { (&raw mut r).cast::<isize>().write_unaligned(node) }; // TVM_GETITEMRECT takes the item in the rect
        send(self.tree.hwnd(), TVM_GETITEMRECT, 1, &mut r as *mut _ as isize);
        let mut pt = POINT { x: (r.left + r.right) / 2, y: (r.top + r.bottom) / 2 };
        unsafe {
            let _ = ClientToScreen(self.tree.hwnd(), &mut pt);
        }
        POINTL { x: pt.x, y: pt.y }
    }

    /// The tree node lit up as a drop target (0: none).
    fn drop_highlight(&self) -> isize {
        send(self.tree.hwnd(), TVM_GETNEXTITEM, TVGN_DROPHILITE as usize, 0)
    }

    fn tree_select(&self, node: isize) {
        send(self.tree.hwnd(), TVM_SELECTITEM, TVGN_CARET as usize, node);
    }

    fn tree_key(&self, vk: VIRTUAL_KEY) {
        send(self.tree.hwnd(), WM_KEYDOWN, vk.0 as usize, 0);
    }

    fn tree_item(&self, node: isize, mask: windows::Win32::UI::Controls::TVITEM_MASK) -> TVITEMW {
        let mut item = TVITEMW { mask, hItem: HTREEITEM(node), ..Default::default() };
        send(self.tree.hwnd(), TVM_GETITEMW, 0, &mut item as *mut _ as isize);
        item
    }

    /// The texts of a node and its next siblings, comma-separated.
    fn node_texts(&self, first: isize) -> String {
        let mut texts = Vec::new();
        let mut node = first;
        while node != 0 {
            let mut text = [0u16; 64];
            let mut item = TVITEMW {
                mask: TVIF_TEXT,
                hItem: HTREEITEM(node),
                pszText: windows::core::PWSTR(text.as_mut_ptr()),
                cchTextMax: 64,
                ..Default::default()
            };
            send(self.tree.hwnd(), TVM_GETITEMW, 0, &mut item as *mut _ as isize);
            texts.push(from_wide(&text).to_string_lossy().into_owned());
            node = self.tree_next(node);
        }
        texts.join(",")
    }

    /// The texts under Bookmarks, the tree's first root.
    fn bookmark_nodes(&self) -> String {
        self.node_texts(self.tree_child(self.tree_root()))
    }

    /// The nodes from `first` on, and all below them.
    fn count_nodes(&self, first: isize) -> usize {
        let mut count = 0;
        let mut node = first;
        while node != 0 {
            count += 1 + self.count_nodes(self.tree_child(node));
            node = self.tree_next(node);
        }
        count
    }

    pub fn start_self_test(&self) {
        let _ = std::fs::write(log_path(), ""); // the log starts empty; every step then appends its line
        let dir = test_dir;
        let mut s = Steps::default();

        // 6 items: the 5 test files and folders, plus the self-test's own Bookmarks folder.
        s.check("opens the start folder with all 6 items", move |a| a.at(&dir()) && a.list.count() == 6);
        s.check("shows a Chinese file name", |a| a.listed("\u{5831}\u{544A}.pdf"));
        s.check("the folder Qoqdir was started from is not kept open (its working folder is the system folder)", |_| {
            let cwd = std::env::current_dir().unwrap_or_default();
            let mut system = [0u16; 260];
            unsafe { GetSystemDirectoryW(Some(&mut system)) };
            note(format!("working folder {}", cwd.display()));
            Location::dir(cwd).same(&Location::dir(from_wide(&system)))
        });
        s.check("a drive without a disk fails quietly, without Windows' \"No Disk\" box", |_| {
            let mode = unsafe { GetErrorMode() };
            mode & SEM_FAILCRITICALERRORS.0 != 0
        });
        s.add(
            "typing in the list filters it",
            |a| {
                focus(a.list.hwnd());
                a.type_text("file");
            },
            |a| a.list.count() == 3 && unsafe { GetFocus() } == a.filter,
        );
        s.add("Esc clears the filter and returns to the list", |a| a.press(VK_ESCAPE, 0), |a| {
            a.list.count() == 6 && unsafe { GetFocus() } == a.list.hwnd()
        });
        // An input method hands over a whole word at once: its letters wait in the queue for the list together.
        s.add(
            "a word an input method types at once goes into the filter box whole, not just its first letter",
            |a| {
                focus(a.list.hwnd());
                for c in "\u{5831}\u{544A}".encode_utf16() {
                    unsafe {
                        let _ = PostMessageW(Some(a.list.hwnd()), WM_CHAR, WPARAM(c as usize), LPARAM(1));
                    }
                }
            },
            |a| {
                note(format!("filter \"{}\"", a.filter_text()));
                a.filter_text() == "\u{5831}\u{544A}" && a.list.count() == 1
            },
        );
        s.add("(Esc clears it)", |a| a.press(VK_ESCAPE, 0), |a| {
            a.list.count() == 6 && unsafe { GetFocus() } == a.list.hwnd()
        });
        // AltGr arrives as Ctrl + Alt: the key must type its character, not fire the Ctrl shortcut.
        s.add(
            "AltGr+L types into the filter instead of focusing the address bar",
            |a| {
                a.press(letter(b'L'), CTRL | ALT); // the key down
                let focus = unsafe { GetFocus() }; // then the character it makes (l-stroke)
                a.on_key(Key { message: WM_CHAR, code: 0x142, focus, ctrl: true, shift: false, alt: true });
            },
            |a| unsafe { GetFocus() } == a.filter && unsafe { GetWindowTextLengthW(a.filter) } == 1 && a.list.count() == 0,
        );
        s.add(
            "Ctrl+Backspace in the filter box deletes the last word, as in Explorer (not a box character)",
            |a| {
                set_text(a.filter, "");
                focus(a.list.hwnd());
                a.type_text("file 1");
                a.press(VK_BACK, CTRL);
            },
            |a| {
                note(format!("filter \"{}\"", a.filter_text()));
                a.filter_text() == "file "
            },
        );
        s.add(
            "Ctrl+C puts the file on the clipboard",
            |a| {
                set_text(a.filter, "");
                focus(a.list.hwnd());
                a.select_one("file1.txt");
                a.press(letter(b'C'), CTRL);
            },
            |_| holds_test_file(),
        );
        s.add(
            "filter + Enter opens a folder",
            |a| {
                a.type_text("sub");
                a.press(VK_RETURN, 0);
            },
            |a| a.at(&p("sub")),
        );
        s.add(
            "Ctrl+V pastes with the Windows copy engine",
            |a| {
                if holds_test_file() {
                    a.press(letter(b'V'), CTRL);
                }
            },
            |_| exists(&p("sub\\file1.txt")),
        )
        .wait(10000);
        s.check("pasted file appears in the list by itself", |a| a.idle() && a.listed("file1.txt"));
        s.add(
            "Paste shortcut makes a shortcut to the copied file",
            |a| {
                if holds_test_file() {
                    paste_into(a.wnd, &a.list.location(), true);
                }
            },
            |_| {
                list_folder(&Location::dir(p("sub")), false, None).items.iter().any(|it| {
                    is_shortcut(&it.name)
                        && shortcut_target(&p("sub").join(&it.name)).is_some_and(|(t, _)| t.same(&Location::dir(p("file1.txt"))))
                })
            },
        )
        .wait(10000);
        s.add("Backspace goes up and selects the folder you came from", |a| a.press(VK_BACK, 0), move |a| {
            a.at(&dir()) && a.focused_is("sub") && a.list.selected_names().len() == 1
        });
        s.add("Alt+Left goes back", |a| a.press(VK_LEFT, ALT), |a| a.at(&p("sub")));
        s.add("Alt+Right goes forward", |a| a.press(VK_RIGHT, ALT), move |a| a.at(&dir()));
        s.add("Ctrl+Shift+N creates a folder and starts renaming it", |a| a.press(letter(b'N'), CTRL | SHIFT), |a| {
            a.idle() && a.list.renaming() && is_directory(&p("New folder"))
        });
        s.add(
            "Enter in the rename box renames the new folder",
            |a| {
                if let Some(edit) = a.list.edit_control() {
                    set_text(edit, "made");
                }
                a.press(VK_RETURN, 0);
            },
            |a| a.idle() && is_directory(&p("made")) && !exists(&p("New folder")) && a.focused_is("made"),
        );
        s.add(
            "Ctrl+Shift+N while a refresh is pending still selects and renames the new folder afterwards",
            |a| {
                focus(a.list.hwnd());
                a.refresh(ViewState::default()); // a load is in flight, as after F5 or a change notification on a slow folder
                a.press(letter(b'N'), CTRL | SHIFT);
            },
            |a| {
                // "New folder (2)" if the folder of the earlier step is still there (its rename could not run)
                a.idle() && a.list.renaming() && a.list.focused_name().is_some_and(|n| n.to_string_lossy().starts_with("New folder"))
            },
        );
        s.add(
            "(cancel that rename and remove the folder)",
            |a| {
                a.cancel_rename();
                let _ = std::fs::remove_dir(p("New folder"));
                let _ = std::fs::remove_dir(p("New folder (2)"));
                a.refresh(ViewState::default());
            },
            |a| a.idle() && !a.listed("New folder") && !a.listed("New folder (2)"),
        );
        s.add(
            "F2 selects the name without its extension",
            |a| {
                focus(a.list.hwnd());
                a.type_text("file2");
                a.press(VK_DOWN, 0);
                a.press(VK_F2, 0);
            },
            |a| {
                let Some(edit) = a.list.edit_control() else { return false };
                let (mut from, mut to) = (0u32, 0u32);
                send(edit, EM_GETSEL, &mut from as *mut _ as usize, &mut to as *mut _ as isize);
                a.edit_text() == "file2.txt" && from == 0 && to == 5
            },
        );
        s.add(
            "typing a new name keeps the extension",
            |a| {
                if let Some(edit) = a.list.edit_control() {
                    send(edit, EM_REPLACESEL, 1, wide("renamed").as_ptr() as isize);
                }
                a.press(VK_RETURN, 0);
            },
            |a| a.idle() && exists(&p("renamed.txt")) && !exists(&p("file2.txt")),
        );
        s.add(
            "a refresh that lands during a rename waits for it",
            |a| {
                set_text(a.filter, "");
                write_file(&p("added.txt")); // sorts first, so every other file moves down one row
                ROWS.set(a.list.count());
                a.refresh(ViewState::default()); // lands while the rename box below is open
                focus(a.list.hwnd());
                a.select_one("\u{5831}\u{544A}.pdf"); // untouched by the steps above
                a.press(VK_F2, 0);
                if let Some(edit) = a.list.edit_control() {
                    set_text(edit, "renamed2.pdf");
                }
            },
            |a| a.idle() && a.list.renaming() && a.edit_text() == "renamed2.pdf" && a.list.count() == ROWS.get(),
        );
        s.add("Enter then renames the right file, and the refresh runs", |a| a.press(VK_RETURN, 0), |a| {
            a.idle()
                && !a.list.renaming()
                && exists(&p("renamed2.pdf"))
                && !exists(&p("\u{5831}\u{544A}.pdf"))
                && a.listed("added.txt")
        });
        s.add(
            "Delete moves the file to the Recycle Bin and selects the item after it",
            |a| {
                set_text(a.filter, ""); // unfiltered, so there is an item after it (4.6)
                focus(a.list.hwnd());
                let next = a.select_one("file10.txt").and_then(|row| a.list.item(row + 1)).map(|it| it.name);
                NEXT.with(|n| *n.borrow_mut() = next.unwrap_or_default());
                a.press(VK_DELETE, 0);
                // Listed until the reload lands, but it must not stay selected (a second Delete would fail).
                if !a.list.selection().is_empty() {
                    note("the deleted file stayed selected until the reload".into());
                }
            },
            |a| {
                let next = NEXT.with(|n| n.borrow().clone());
                a.idle() && !exists(&p("file10.txt")) && a.list.selected_names() == [next] && noted().is_empty()
            },
        )
        .wait(10000);
        s.add(
            "files created by other programs appear by themselves, and the selection stays (4.5)",
            |a| {
                set_text(a.filter, "");
                a.select_one("file1.txt");
                write_file(&p("external.txt"));
            },
            |a| a.idle() && a.listed("external.txt") && a.focused_is("file1.txt"),
        );
        s.add(
            "opening a folder",
            |a| {
                let _ = std::fs::create_dir(p("gone"));
                a.open(Location::dir(p("gone")), Nav::New, ViewState::default());
            },
            |a| a.at(&p("gone")),
        );
        let gone_shown = |a: &App| a.at(&p("gone")) && a.list.count() == 0 && a.status_message().contains("not available");
        s.add(
            "...that another program deletes stays in its tab, shown as not available",
            |_| {
                let _ = std::fs::remove_dir(p("gone"));
            },
            gone_shown,
        );
        // The tab keeps its gone folder (4.3), which a command must not get as its working folder.
        s.add("(F5 while it is still gone keeps it shown as not available)", |a| a.press(VK_F5, 0), gone_shown);
        s.add("a command typed there still runs (the dead folder is not its working folder)", |a| a.go_to("rundll32"), |a| {
            a.at(&p("gone")) && a.status_message().contains("not available")
        });
        s.add(
            "F5 tries again once it is back",
            |a| {
                let _ = std::fs::create_dir(p("gone"));
                a.press(VK_F5, 0);
            },
            |a| a.at(&p("gone")) && a.status_message().is_empty(),
        );
        s.add(
            "(back up to the test folder)",
            |a| {
                focus(a.list.hwnd());
                a.press(VK_BACK, 0);
            },
            move |a| a.at(&dir()),
        );
        s.add("Ctrl+T opens a tab of the same folder", |a| a.press(letter(b'T'), CTRL), move |a| {
            a.idle() && a.tabs() == 2 && a.at(&dir())
        });
        s.add("Ctrl+W closes it", |a| a.press(letter(b'W'), CTRL), move |a| a.idle() && a.tabs() == 1 && a.at(&dir()));
        s.add("Ctrl+W on the last tab keeps it (the last tab cannot be closed)", |a| a.press(letter(b'W'), CTRL), move |a| {
            a.idle() && a.tabs() == 1 && a.at(&dir())
        });
        s.add(
            "Ctrl+Shift+PgDn moves the tab one place right, and no further; it stays the current tab",
            |a| {
                a.new_tab(Location::dir(p("sub")), false); // a second tab, right of this one
                a.press(VK_NEXT, CTRL | SHIFT);
                a.press(VK_NEXT, CTRL | SHIFT);
            },
            move |a| {
                a.tabs() == 2
                    && a.tab_path(0).same(&Location::dir(p("sub")))
                    && a.active() == 1
                    && send(a.tab_bar, TCM_GETCURSEL, 0, 0) == 1
                    && a.at(&dir())
            },
        );
        s.add(
            "Ctrl+Shift+PgUp moves it back, and no further",
            |a| {
                a.press(VK_PRIOR, CTRL | SHIFT);
                a.press(VK_PRIOR, CTRL | SHIFT);
            },
            move |a| {
                a.tabs() == 2
                    && a.tab_path(1).same(&Location::dir(p("sub")))
                    && a.active() == 0
                    && send(a.tab_bar, TCM_GETCURSEL, 0, 0) == 0
                    && a.at(&dir())
            },
        );
        s.add("(close that tab)", |a| a.close_tab(1), move |a| a.idle() && a.tabs() == 1 && a.at(&dir()));
        s.add(
            "pressing a tab makes it current at once, and dragging it along the bar moves it past the others \
             (a slip just into the next tab moves nothing)",
            |a| {
                a.new_tab(Location::dir(p("sub")), false);
                a.new_tab(Location::ThisPc, false); // tabs: the test folder, sub, This PC
                drag_along_bar(a, 0, tab_rect(a, 1).left + 1); // a slip during a click, short of the next tab's middle
                drag_along_bar(a, 1, tab_rect(a, 2).right - 1);
            },
            move |a| {
                a.tabs() == 3
                    && a.tab_path(0).same(&Location::dir(dir()))
                    && matches!(a.tab_path(1), Location::ThisPc)
                    && a.active() == 2
                    && send(a.tab_bar, TCM_GETCURSEL, 0, 0) == 2
                    && a.at(&p("sub"))
            },
        );
        s.add(
            "dragging it back to the start puts it first",
            |a| drag_along_bar(a, 2, tab_rect(a, 0).left + 1),
            move |a| {
                a.tab_path(1).same(&Location::dir(dir()))
                    && matches!(a.tab_path(2), Location::ThisPc)
                    && a.active() == 0
                    && send(a.tab_bar, TCM_GETCURSEL, 0, 0) == 0
                    && a.at(&p("sub"))
            },
        );
        s.add(
            "the mouse wheel over the tabs shows the next tab (a touchpad's small steps add up) and stops at the last",
            |a| {
                for _ in 0..4 {
                    wheel(a, -30); // four small steps make one
                }
                wheel(a, -120);
                wheel(a, -120); // past the last tab: nothing
            },
            |a| a.idle() && a.active() == 2 && a.at_pc(),
        );
        s.add(
            "turning the wheel the other way goes back to the first tab, and stops there",
            |a| {
                for _ in 0..3 {
                    wheel(a, 120);
                }
            },
            |a| a.idle() && a.active() == 0 && a.at(&p("sub")),
        );
        s.check("the current tab's name, and only its, starts with a bullet", |a| {
            (0..a.tabs()).all(|i| caption(a, i).starts_with(&[0x2022, 0x20]) == (i == a.active()))
        });
        s.add("a new tab goes at the end, so the newest is the last one", |a| a.press(letter(b'T'), CTRL), move |a| {
            a.tabs() == 4
                && a.tab_path(1).same(&Location::dir(dir()))
                && matches!(a.tab_path(2), Location::ThisPc)
                && a.active() == 3
                && a.at(&p("sub"))
        });
        // Keeps the test folder's own tab.
        s.add("(close the other tabs)", |a| a.close_other_tabs(1), move |a| a.idle() && a.tabs() == 1 && a.at(&dir()));
        s.check("a lone tab is 150 wide at 100% scaling", |a| tab_width(a) == scale(150, a.dpi.get()));
        let long_name = || Location::dir(p("a folder with a name too long for its tab")); // never opened: only its caption
        s.add(
            "when more tabs are open than fit that wide, all get narrower alike, and all still show",
            move |a| {
                while a.tabs() as i32 <= bar_width(a) / scale(150, a.dpi.get()) {
                    a.new_tab(long_name(), false);
                }
            },
            |a| {
                let (width, dpi) = (tab_width(a), a.dpi.get());
                note(format!("{} tabs {width} wide", a.tabs()));
                width >= scale(80, dpi) && width < scale(150, dpi) && tab_rect(a, a.tabs() - 1).right <= bar_width(a)
            },
        );
        s.add(
            "...but no narrower than 80; with more, the bar scrolls",
            move |a| {
                while a.tabs() as i32 <= bar_width(a) / scale(80, a.dpi.get()) {
                    a.new_tab(long_name(), false);
                }
            },
            |a| {
                let arrows = unsafe { FindWindowExW(Some(a.tab_bar), None, UPDOWN_CLASSW, windows::core::PCWSTR::null()) };
                tab_width(a) == scale(80, a.dpi.get()) && arrows.is_ok_and(|h| unsafe { IsWindowVisible(h) }.as_bool())
            },
        );
        s.add("(close them)", |a| a.close_other_tabs(a.active()), move |a| a.idle() && a.tabs() == 1 && a.at(&dir()));
        s.add(
            "a tab the control selects by itself (as assistive tools select one) switches to its folder",
            |a| {
                a.new_tab(Location::dir(p("sub")), false); // a second tab, right of this one
                send(a.tab_bar, TCM_SETCURFOCUS, 1, 0); // on plain tabs this selects, and notifies the window as a click would
            },
            |a| a.idle() && a.active() == 1 && a.at(&p("sub")),
        );
        s.add("(close that tab)", |a| a.close_tab(1), move |a| a.idle() && a.tabs() == 1 && a.at(&dir()));
        // What Windows sends when the window reaches a monitor with another scaling: the dots per inch there, and where
        // the window goes.
        let dpi_change = |a: &App, dpi: u32, at: RECT| send(a.wnd, WM_DPICHANGED, (dpi | dpi << 16) as usize, &at as *const _ as isize);
        s.add(
            "a move to a monitor with another scaling (as Windows reports it) rescales the font, the columns and the tabs",
            move |a| {
                let mut at = RECT::default();
                unsafe {
                    let _ = GetWindowRect(a.wnd, &mut at);
                }
                BEFORE.set((a.dpi.get(), a.row_height.get(), a.list.column_widths()[0], at));
                let bigger = RECT { right: at.left + (at.right - at.left) * 4 / 3, bottom: at.top + (at.bottom - at.top) * 4 / 3, ..at };
                dpi_change(a, a.dpi.get() * 4 / 3, bigger);
            },
            |a| {
                let (dpi0, row0, col0, _) = BEFORE.get();
                let dpi = dpi0 * 4 / 3;
                a.dpi.get() == dpi
                    && a.row_height.get() > row0
                    && a.list.column_widths()[0] == (col0 as i64 * dpi as i64 / dpi0 as i64) as i32
                    && tab_width(a) == scale(150, dpi)
            },
        );
        s.add(
            "...and the move back restores them",
            move |a| {
                let (dpi0, _, _, at0) = BEFORE.get();
                dpi_change(a, dpi0, at0);
            },
            |a| {
                let (dpi0, row0, col0, _) = BEFORE.get();
                let column = a.list.column_widths()[0] - col0; // within rounding
                a.dpi.get() == dpi0 && a.row_height.get() == row0 && (-1..=1).contains(&column) && tab_width(a) == scale(150, dpi0)
            },
        );
        // The test must not switch the user's theme: it applies the colours for one directly.
        s.add(
            "in a high-contrast theme the window, list and tree take the theme's colours; the soft gray comes back after",
            |a| {
                a.apply_colors(true);
                let window = unsafe { GetSysColor(COLOR_WINDOW) } as isize;
                let brush = unsafe { GetClassLongPtrW(a.wnd, GCLP_HBRBACKGROUND) } as isize;
                let themed = send(a.list.hwnd(), LVM_GETBKCOLOR, 0, 0) == window
                    && send(a.list.hwnd(), LVM_GETTEXTBKCOLOR, 0, 0) == window
                    && send(a.tree.hwnd(), TVM_GETBKCOLOR, 0, 0) == window
                    && brush == unsafe { GetSysColorBrush(COLOR_WINDOW) }.0 as isize;
                note(if themed { "theme colours" } else { "still gray" }.into());
                a.apply_colors(false);
            },
            |a| {
                let gray = 0x00EBEBEB;
                noted() == "theme colours"
                    && send(a.list.hwnd(), LVM_GETBKCOLOR, 0, 0) == gray
                    && send(a.tree.hwnd(), TVM_GETBKCOLOR, 0, 0) == gray
            },
        );
        s.add(
            "switching tabs shows the new tab at once; nothing acts on the old one meanwhile",
            |a| {
                a.new_tab(Location::dir(p("sub")), false);
                focus(a.list.hwnd());
                a.switch_tab(a.active() + 1);
                // Before "sub" can have loaded: the address bar names it, and Ctrl+Shift+N must not hit the old tab.
                let switched = Location::dir(window_text(a.address)).same(&Location::dir(p("sub")));
                note(if switched { "address bar switched" } else { "old tab still shown" }.into());
                a.press(letter(b'N'), CTRL | SHIFT);
            },
            |a| {
                a.at(&p("sub"))
                    && noted() == "address bar switched"
                    && !exists(&p("New folder"))
                    && !exists(&p("sub\\New folder"))
            },
        );
        s.add("(close that tab)", |a| a.close_tab(a.active()), move |a| a.idle() && a.tabs() == 1 && a.at(&dir()));
        s.add(
            "a typed path that fails while a rename box is open leaves the box alone",
            |a| {
                // The failure lands after the rename box opens (a dead share fails a minute later).
                a.go_to("C:\\no\\such\\folder\\anywhere");
                a.select_one("file1.txt");
                a.press(VK_F2, 0);
            },
            move |a| a.at(&dir()) && a.list.renaming() && unsafe { GetFocus() } != a.address,
        );
        s.add(
            "a refresh asked to select an item, landing during a rename, keeps that for later",
            |a| {
                a.cancel_rename(); // the box from the step before
                focus(a.list.hwnd());
                a.select_one("file1.txt");
                a.refresh(ViewState::selecting("added.txt")); // as after a rename or a new folder; the load is in flight
                a.press(VK_F2, 0); // the box opens before it lands
            },
            |a| a.idle() && a.list.renaming(),
        );
        s.add("...and once the box closes the queued refresh selects it", |a| a.cancel_rename(), |a| {
            note(format!("focused {:?}", a.list.focused_name().unwrap_or_default()));
            a.idle() && !a.refresh_queued.get() && a.focused_is("added.txt")
        });
        s.add(
            "a newer request to select an item replaces an older one still waiting",
            move |a| {
                // An older request, waiting (a slow folder); then a newer one, as after a second rename.
                a.st.borrow_mut().queued_view = Some((Location::dir(dir()), ViewState::selecting("file1.txt")));
                a.refresh(ViewState::selecting("added.txt"));
            },
            |a| a.idle() && a.focused_is("added.txt"),
        );
        s.add(
            "...so the waiting refresh, when it runs, leaves the newer item selected",
            |a| {
                send(a.wnd, WM_TIMER, TIMER_REFRESH, 0);
            },
            |a| {
                note(format!("focused {:?}", a.list.focused_name().unwrap_or_default()));
                a.idle() && a.focused_is("added.txt")
            },
        );
        s.add("a command that does not exist says Can't find", |a| a.go_to("no-such-program-qoqdir"), move |a| {
            let (mut from, mut to) = (0u32, 0u32); // the text stays selected, so it can be corrected (5.3)
            send(a.address, EM_GETSEL, &mut from as *mut _ as usize, &mut to as *mut _ as isize);
            a.at(&dir())
                && a.status_message().contains("Can't find")
                && from == 0
                && to as i32 == unsafe { GetWindowTextLengthW(a.address) }
        });
        s.add(
            "a program typed by name is Windows' own, not a file of that name in the folder shown",
            |a| {
                write_file(&p("rundll32.exe")); // not a program: run from here, it would fail
                a.set_message("");
                a.go_to("rundll32");
                let _ = std::fs::remove_file(p("rundll32.exe"));
            },
            move |a| {
                note(format!("status: {}", a.status_message()));
                a.at(&dir()) && a.status_message().is_empty()
            },
        );

        s.add("(open This PC)", |a| a.open(Location::ThisPc, Nav::New, ViewState::default()), |a| a.at_pc());
        s.add(
            "typing . on This PC says Can't find (not handed to Windows, which opens Explorer)",
            |a| {
                a.set_message("");
                a.go_to(".");
            },
            |a| {
                note(format!("status: {}", a.status_message()));
                a.at_pc() && a.status_message().contains("Can't find")
            },
        );
        s.home("(back to the test folder from This PC)");

        s.add("typing a folder path opens it", |a| a.go_to(p("sub")), |a| a.at(&p("sub")));
        s.add(
            "typing an unreachable network path does not freeze the window",
            |a| {
                set_text(a.address, "\\\\192.0.2.1\\share"); // reserved test address: nothing answers
                focus(a.address);
                let t = Instant::now();
                a.press(VK_RETURN, 0);
                KEY_MS.set(t.elapsed().as_millis());
                note(format!("Enter took {} ms", KEY_MS.get()));
            },
            |_| KEY_MS.get() < 500,
        );
        s.add(
            "...and opening another folder cancels it",
            move |a| a.open(Location::dir(dir()), Nav::New, ViewState::default()),
            move |a| a.at(&dir()),
        )
        .during_load();
        s.add("typing a subfolder's name opens it here (not in Explorer)", |a| a.go_to("sub"), |a| a.at(&p("sub")));
        s.add("typing ..\\ goes up", |a| a.go_to("..\\"), move |a| a.at(&dir()));
        s.add("typing .\\sub opens it here (not in Explorer)", |a| a.go_to(".\\sub"), |a| a.at(&p("sub")));
        s.add(
            "a long folder name of emojis is cut to fit its tab, ending in an ellipsis, with no broken character",
            |a| {
                let name = "\u{1F600}".repeat(40); // each is two UTF-16 units; the cut lands among them
                let _ = std::fs::create_dir(p(&name));
                a.open(Location::dir(p(&name)), Nav::New, ViewState::default());
            },
            |a| {
                if !a.idle() || a.tab_location().display_name().encode_wide().count() != 80 {
                    return false;
                }
                let caption = caption(a, a.active());
                // Every high surrogate must be followed by its low one.
                let whole = caption.iter().enumerate().all(|(i, &c)| {
                    !(0xD800..0xDC00).contains(&c) || caption.get(i + 1).is_some_and(|n| (0xDC00..0xE000).contains(n))
                });
                whole && caption.len() < 80 && caption.last() == Some(&0x2026)
            },
        );
        s.home("(back up to the test folder)");

        // A folder whose name looks like an environment variable: only what the user types is expanded.
        s.add(
            "(open a folder literally named %TEMP%, with sub and other in it)",
            |a| {
                for name in ["%TEMP%", "%TEMP%\\sub", "%TEMP%\\other"] {
                    let _ = std::fs::create_dir(p(name));
                }
                a.open(Location::dir(p("%TEMP%")), Nav::New, ViewState::default());
            },
            |a| a.at(&p("%TEMP%")) && a.list.count() == 2,
        );
        s.add(
            "typing a subfolder's name in a folder named %TEMP% opens it (the folder's own path is not expanded)",
            |a| a.go_to("sub"),
            |a| {
                note(format!("tab at {}, status: {}", a.tab_location().text().to_string_lossy(), a.status_message()));
                a.at(&p("%TEMP%\\sub"))
            },
        );
        s.add(
            "(back to the %TEMP% folder)",
            |a| a.open(Location::dir(p("%TEMP%")), Nav::New, ViewState::default()),
            |a| a.at(&p("%TEMP%")) && a.list.count() == 2,
        );
        s.add(
            "Enter in the filter box opens only the highlighted match, not every selected one (5.4)",
            |a| {
                focus(a.list.hwnd());
                a.list.select_all(); // both folders selected, "sub" highlighted (it was just left)
                a.type_text("*"); // a filter both still match
                a.press(VK_RETURN, 0);
            },
            |a| {
                note(format!("tab at {}, {} tab(s)", a.tab_location().text().to_string_lossy(), a.tabs()));
                a.at(&p("%TEMP%\\sub")) && a.tabs() == 1
            },
        );
        s.add(
            "(close any tabs that opened)",
            |a| {
                while a.tabs() > 1 {
                    a.close_tab(a.tabs() - 1);
                }
            },
            |a| a.idle() && a.tabs() == 1,
        );
        s.home("(back to the test folder)");
        s.add(
            "(remove the %TEMP% folder)",
            |a| {
                for name in ["%TEMP%\\sub", "%TEMP%\\other", "%TEMP%"] {
                    let _ = std::fs::remove_dir(p(name));
                }
                a.refresh(ViewState::default());
            },
            move |a| a.at(&dir()) && !a.list.has_folder(OsStr::new("%TEMP%")),
        );

        // Hidden items, the filter during a load, a refresh queued before a failure.
        s.add(
            "(add a hidden file and a hidden folder)",
            |a| {
                write_file(&p("secret.txt"));
                let _ = std::fs::create_dir(p("hid"));
                for name in ["secret.txt", "hid"] {
                    let path = wide(p(name));
                    unsafe {
                        let attrs = GetFileAttributesW(pcwstr(&path));
                        let _ = SetFileAttributesW(pcwstr(&path), FILE_FLAGS_AND_ATTRIBUTES(attrs | FILE_ATTRIBUTE_HIDDEN.0));
                    }
                }
                a.refresh(ViewState::default());
            },
            |a| a.idle() && !a.listed("secret.txt") && !a.listed("hid"),
        );
        s.add(
            "Ctrl+H shows hidden files at once, without reading the folder again",
            |a| {
                COUNT.set(a.generation.get());
                focus(a.list.hwnd());
                a.press(letter(b'H'), CTRL);
            },
            |a| {
                note(format!("{} read(s)", a.generation.get() - COUNT.get()));
                a.idle() && a.listed("secret.txt") && a.generation.get() == COUNT.get()
            },
        );
        s.add("(Ctrl+H hides them again)", |a| a.press(letter(b'H'), CTRL), |a| a.idle() && !a.listed("secret.txt"));
        s.add("typing a hidden folder's name opens it here (not in Explorer)", |a| a.go_to("hid"), |a| a.at(&p("hid")));
        s.add(
            "letters typed while a folder loads stay in the filter box when it lands (5.4)",
            move |a| {
                a.open(Location::dir(dir()), Nav::New, ViewState::default()); // back up from "hid"
                focus(a.list.hwnd());
                a.type_text("file");
            },
            move |a| {
                note(format!("filter \"{}\"", a.filter_text()));
                a.at(&dir()) && a.filter_text() == "file"
            },
        );
        s.add(
            "a folder that fails to open leaves the filter of the folder still shown",
            |a| a.open(Location::dir(p("no-such-folder")), Nav::New, ViewState::default()),
            move |a| {
                note(format!("filter \"{}\"", a.filter_text()));
                a.at(&dir()) && a.filter_text() == "file"
            },
        );
        s.add(
            "a refresh queued just before a tab became not available waits for F5 (4.3)",
            move |a| {
                a.queue_refresh(); // a change in the folder, just before
                a.show_unavailable(&Location::dir(dir()), "The network path was not found.", Nav::Refresh); // then it became unreachable
                send(a.wnd, WM_TIMER, TIMER_REFRESH, 0); // and the queued refresh fires
            },
            |a| a.idle() && a.unavailable() && a.status_message().contains("not available"),
        );
        s.add("(F5 shows it again)", |a| a.press(VK_F5, 0), move |a| a.at(&dir()) && !a.unavailable());

        // History entries for deleted folders, and shortcuts.
        s.add(
            "(open a folder that will be deleted)",
            |a| {
                let _ = std::fs::create_dir(p("temp1"));
                a.open(Location::dir(p("temp1")), Nav::New, ViewState::default());
            },
            |a| a.at(&p("temp1")),
        );
        s.home("(go back to the test folder)");
        s.add(
            "Back to a deleted folder drops it from history instead of getting stuck",
            |a| {
                let _ = std::fs::remove_dir(p("temp1"));
                a.press(VK_LEFT, ALT);
            },
            move |a| {
                let last_back = {
                    let st = a.st.borrow();
                    st.tabs[st.active].back.last().cloned()
                };
                a.at(&dir()) && !last_back.is_some_and(|l| l.same(&Location::dir(p("temp1"))))
            },
        );
        // Enter on the selected shortcuts, once the refresh that selected them has landed.
        let enter_when = |names: &'static [&'static str]| {
            move |a: &App| {
                let selected = a.list.selected_names();
                if !a.idle() || selected.len() != names.len() || !names.iter().all(|n| selected.iter().any(|s| s == *n)) {
                    return false;
                }
                focus(a.list.hwnd());
                a.press(VK_RETURN, 0);
                true
            }
        };
        s.add(
            "a shortcut to a folder opens it here",
            |a| {
                make_shortcut(&p("sub-link.lnk"), p("sub"));
                a.refresh(ViewState::selecting("sub-link.lnk"));
            },
            enter_when(&["sub-link.lnk"]),
        );
        s.check("(the shortcut's folder is shown)", |a| a.at(&p("sub")));
        s.home("(back up to the test folder)");
        let clean = |l: Location| !l.text().to_string_lossy().ends_with('\\'); // else the caption and Up break
        s.add(
            "a shortcut whose target ends with a backslash opens the folder with a clean path",
            |a| {
                make_shortcut(&p("slash.lnk"), with_slash(p("sub")));
                a.open_shortcut(p("slash.lnk"), false);
            },
            move |a| a.at(&p("sub")) && clean(a.tab_location()),
        );
        s.home("(back up to the test folder)");
        s.add(
            "Ctrl+Enter on a shortcut to a folder opens it in a new background tab (not in Explorer)",
            |a| {
                a.select_one("sub-link.lnk");
                focus(a.list.hwnd());
                a.press(VK_RETURN, CTRL);
            },
            move |a| a.tabs() == 2 && a.tab_path(a.active() + 1).same(&Location::dir(p("sub"))) && a.at(&dir()),
        );
        s.add("(close that tab)", |a| a.close_tab(a.active() + 1), |a| a.idle() && a.tabs() == 1);
        s.add(
            "Enter on two shortcuts to folders, one whose target ends with a backslash, opens both in background tabs",
            |a| {
                make_shortcut(&p("slash2.lnk"), with_slash(p("sub")));
                let names = vec!["sub-link.lnk".into(), "slash2.lnk".into()];
                a.refresh(ViewState { select: names, focus: Some("sub-link.lnk".into()), ..Default::default() });
            },
            enter_when(&["sub-link.lnk", "slash2.lnk"]),
        );
        s.check("(both are in tabs behind the test folder, with clean paths, so with captions)", move |a| {
            if a.tabs() != 3 || !a.at(&dir()) {
                return false;
            }
            note(format!("tab paths: {} | {}", a.tab_path(1).text().to_string_lossy(), a.tab_path(2).text().to_string_lossy()));
            let sub = Location::dir(p("sub"));
            a.tab_path(1).same(&sub) && a.tab_path(2).same(&sub) // not "sub\"
        });
        s.add(
            "(close those tabs)",
            |a| {
                a.close_tab(1);
                a.close_tab(1);
            },
            move |a| a.idle() && a.tabs() == 1 && a.at(&dir()),
        );
        s.add(
            "in a network folder that is not available, paste, new folder and a drag over its tab return at once",
            |a| {
                // As if this tab's folder had just failed to load (a real failure can take minutes to arrive).
                let offline = Location::dir("\\\\192.0.2.1\\share");
                {
                    let mut st = a.st.borrow_mut();
                    let active = st.active;
                    st.tabs[active].location = offline.clone();
                }
                a.show_unavailable(&offline, "The network path was not found.", Nav::Refresh);
                let cursor = a.tab_point(a.active());
                let data = data_object_at(a.wnd, &Location::dir(p("file1.txt")));
                focus(a.list.hwnd());
                let t = Instant::now();
                a.press(letter(b'V'), CTRL);
                a.press(letter(b'N'), CTRL | SHIFT);
                if let (Some(data), Some(target)) = (data, a.tab_drop.borrow().clone()) {
                    let mut effect = DROPEFFECT_COPY;
                    unsafe {
                        let _ = target.DragEnter(&data, MK_LBUTTON, cursor, &mut effect);
                        let _ = target.DragLeave();
                    }
                }
                KEY_MS.set(t.elapsed().as_millis());
                note(format!("took {} ms", KEY_MS.get()));
            },
            |_| KEY_MS.get() < 100,
        );
        let reopen_home = move |a: &App| {
            {
                let mut st = a.st.borrow_mut();
                let active = st.active;
                st.tabs[active].location = Location::dir(dir());
            }
            a.open(Location::dir(dir()), Nav::Refresh, ViewState::default());
        };
        s.add("(back to the test folder)", reopen_home, move |a| a.at(&dir()));
        s.add(
            "a shortcut to a folder that is gone says so here (no Windows dialog)",
            |a| {
                make_shortcut(&p("gone.lnk"), p("no-such-folder"));
                a.refresh(ViewState::selecting("gone.lnk"));
            },
            enter_when(&["gone.lnk"]),
        );
        s.check("(the status bar names the missing folder, and the tab stays put)", move |a| {
            a.at(&dir()) && a.status_message().contains("no-such-folder")
        });
        s.add(
            "a shortcut to This PC opens This PC here (not in Explorer)",
            |a| {
                make_shortcut(&p("pc.lnk"), "");
                a.refresh(ViewState::selecting("pc.lnk"));
            },
            enter_when(&["pc.lnk"]),
        );
        s.check("(This PC is shown, with its drives)", |a| a.at_pc() && a.list.count() > 0);
        s.home("(back to the test folder)");

        // Regression tests for bugs fixed earlier.
        s.add(
            "holding F5 while a folder loads does not start more loads",
            |a| {
                let before = a.generation.get();
                a.open(Location::dir(p("sub")), Nav::New, ViewState::default());
                focus(a.list.hwnd());
                for _ in 0..20 {
                    a.press(VK_F5, 0); // like a held-down key
                }
                note(format!("{} load(s) started", a.generation.get() - before));
            },
            |a| a.at(&p("sub")) && noted() == "1 load(s) started",
        );
        s.home("(back up to the test folder again)");
        s.add(
            "a background refresh never cancels a folder being opened",
            |a| {
                a.open(Location::dir(p("sub")), Nav::New, ViewState::default());
                send(a.wnd, WM_TIMER, TIMER_REFRESH, 0); // auto-refresh fires while "sub" is loading
            },
            |a| a.at(&p("sub")),
        );
        s.add(
            "\"Loading...\" goes away after a slow refresh",
            |a| {
                a.refresh(ViewState::default());
                send(a.wnd, WM_TIMER, TIMER_LOADING, 0); // as if the refresh took over 250 ms
            },
            |a| a.idle() && a.status_message() != "Loading\u{2026}",
        );
        s.add(
            "back up to the test folder",
            |a| {
                focus(a.list.hwnd());
                a.press(VK_BACK, 0);
            },
            move |a| a.at(&dir()),
        );
        s.add(
            "(open a folder two levels down)",
            |a| {
                let _ = std::fs::create_dir(p("sub\\deep"));
                a.open(Location::dir(p("sub\\deep")), Nav::New, ViewState::default());
            },
            |a| a.at(&p("sub\\deep")),
        );
        s.add(
            "a removed drive is noticed by a tab two folders deep (shown as not available)",
            |a| {
                // Two levels down only WM_DEVICECHANGE tells: drop the watch, remove the folder, send it.
                a.st.borrow_mut().watch = None;
                let _ = std::fs::remove_dir(p("sub\\deep"));
                volume_change(a, DBT_DEVICEREMOVECOMPLETE);
            },
            |a| a.at(&p("sub\\deep")) && a.list.count() == 0 && a.status_message().contains("not available"),
        );
        s.add("(back to the test folder)", reopen_home, move |a| a.at(&dir()));

        // Slow lookups happen on worker threads (so network drives cannot freeze the window).
        s.add(
            "a program's own icon arrives from the background",
            |a| {
                let system = std::env::var_os("SystemRoot").map(PathBuf::from).unwrap_or_default().join("System32");
                let _ = std::fs::copy(system.join("cmd.exe"), p("tool.exe"));
                a.refresh(ViewState::default());
            },
            |a| {
                let mut sfi = SHFILEINFOW::default();
                unsafe {
                    SHGetFileInfoW(pcwstr(&wide(p("tool.exe"))), FILE_FLAGS_AND_ATTRIBUTES(0), Some(&mut sfi), size_of::<SHFILEINFOW>() as u32, SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
                }
                let Some(row) = (0..a.list.count()).find(|&r| a.list.item(r).is_some_and(|i| i.name == "tool.exe")) else {
                    return false;
                };
                let mut item = LVITEMW { mask: LVIF_IMAGE, iItem: row as i32, ..Default::default() };
                send(a.list.hwnd(), LVM_GETITEMW, 0, &mut item as *mut _ as isize);
                a.idle() && item.iImage == sfi.iIcon
            },
        );
        s.add(
            "sorting by Type puts the Type column in order",
            |a| {
                make_shortcut(&p("sub-link2.lnk"), p("sub")); // a few different types
                write_file(&p("archive.zip"));
                write_file(&p("page.html"));
                a.refresh(ViewState::default());
            },
            |a| {
                if !a.idle() || a.list.total() < 8 {
                    return false;
                }
                a.list.set_sort(SortKey::Type, true);
                let items: Vec<_> = (0..a.list.count()).filter_map(|r| a.list.item(r)).collect();
                let ordered = items.windows(2).all(|w| {
                    let (x, y) = (wide(type_name(&w[0])), wide(type_name(&w[1])));
                    w[0].is_dir() != w[1].is_dir() || unsafe { StrCmpLogicalW(pcwstr(&x), pcwstr(&y)) } <= 0
                });
                a.list.set_sort(SortKey::Name, true);
                ordered
            },
        );
        s.check("the Bookmarks folder is created and shown at the top of the tree", |a| {
            is_directory(&p("Bookmarks")) && a.node_texts(a.tree_root()).split(',').next() == Some("Bookmarks")
        });
        s.check("right-click menus can be built for This PC, a drive and a file", |a| {
            item_menu_at(a.wnd, &Location::ThisPc).is_some()
                && item_menu_at(a.wnd, &Location::dir("C:\\")).is_some()
                && item_menu_at(a.wnd, &Location::dir(p("file1.txt"))).is_some()
        });
        s.add(
            "a drag over a tab whose folder was never shown is refused at once (nobody has checked that folder)",
            |a| {
                a.new_tab(Location::dir(p("made")), false); // a second tab, right of the current one, as from -t or a middle-click
                let cursor = a.tab_point(a.active() + 1);
                let data = data_object_at(a.wnd, &Location::dir(p("file1.txt")));
                let mut effect = DROPEFFECT_COPY;
                let t = Instant::now();
                if let (Some(data), Some(target)) = (data, a.tab_drop.borrow().clone()) {
                    unsafe {
                        let _ = target.DragEnter(&data, MK_LBUTTON, cursor, &mut effect);
                        let mut over = DROPEFFECT_COPY;
                        let _ = target.DragOver(MK_LBUTTON, cursor, &mut over);
                        effect |= over; // refused as the drag enters, and as it moves
                        let _ = target.DragLeave();
                    }
                }
                KEY_MS.set(t.elapsed().as_millis());
                EFFECT.set(effect.0);
                note(format!("took {} ms", KEY_MS.get()));
            },
            |_| EFFECT.get() == DROPEFFECT_NONE.0 && KEY_MS.get() < 100,
        );
        s.add("(show that tab once)", |a| a.switch_tab(a.active() + 1), |a| a.at(&p("made")));
        s.add("(and come back)", |a| a.switch_tab(a.active() - 1), move |a| a.at(&dir()));
        s.add(
            "dropping a file on a tab puts it in that tab's folder; while the file is over it, that tab shows as the \
             current one, and afterwards the real one does again",
            |a| {
                let cursor = a.tab_point(a.active() + 1);
                let data = data_object_at(a.wnd, &Location::dir(p("file1.txt"))); // what a drag carries
                let (Some(data), Some(target)) = (data, a.tab_drop.borrow().clone()) else { return };
                unsafe {
                    let mut effect = DROPEFFECT_COPY;
                    let _ = target.DragEnter(&data, MK_LBUTTON, cursor, &mut effect);
                    effect = DROPEFFECT_COPY;
                    let _ = target.DragOver(MK_LBUTTON, cursor, &mut effect);
                    let shown = send(a.tab_bar, TCM_GETCURSEL, 0, 0) == a.active() as isize + 1;
                    note(if shown { "shown while over it" } else { "not shown while over it" }.into());
                    effect = DROPEFFECT_COPY;
                    let _ = target.Drop(&data, MK_LBUTTON, cursor, &mut effect);
                }
            },
            |a| {
                exists(&p("made\\file1.txt"))
                    && a.drop_tab.get().is_none()
                    && noted() == "shown while over it"
                    && send(a.tab_bar, TCM_GETCURSEL, 0, 0) == a.active() as isize
            },
        )
        .wait(10000);
        s.add("(close the extra tab)", |a| a.close_tab(a.active() + 1), |a| a.idle() && a.tabs() == 1);
        s.add(
            "middle-click on a folder opens it in a new background tab",
            |a| {
                let row = (0..a.list.count()).find(|&r| a.list.item(r).is_some_and(|i| i.name == "sub"));
                if let Some(row) = row {
                    let mut r = RECT { left: LVIR_LABEL as i32, ..Default::default() };
                    send(a.list.hwnd(), LVM_GETITEMRECT, row, &mut r as *mut _ as isize);
                    let packed = ((r.left + r.right) / 2) as u16 as isize | ((((r.top + r.bottom) / 2) as u16 as isize) << 16);
                    send(a.list.hwnd(), WM_MBUTTONUP, 0, packed);
                }
            },
            move |a| a.tabs() == 2 && a.tab_path(a.active() + 1).same(&Location::dir(p("sub"))) && a.at(&dir()),
        );
        s.add("(close that tab)", |a| a.close_tab(a.active() + 1), |a| a.idle() && a.tabs() == 1);
        s.add(
            "middle-click in the tree opens a new background tab",
            |a| {
                let desktop = a.tree_next(a.tree_root()); // Desktop (after Bookmarks)
                let mut r = RECT::default();
                unsafe { (&raw mut r).cast::<isize>().write_unaligned(desktop) }; // TVM_GETITEMRECT takes the item in the rect
                send(a.tree.hwnd(), TVM_GETITEMRECT, 1, &mut r as *mut _ as isize);
                let packed = ((r.left + r.right) / 2) as u16 as isize | ((((r.top + r.bottom) / 2) as u16 as isize) << 16);
                send(a.tree.hwnd(), WM_MBUTTONUP, 0, packed);
            },
            move |a| {
                let desktop = known_folder(&FOLDERID_Desktop).map(Location::dir).unwrap_or(Location::ThisPc);
                a.tabs() == 2 && a.tab_path(a.active() + 1).same(&desktop) && a.at(&dir())
            },
        );
        s.add("(close that tab too)", |a| a.close_tab(a.active() + 1), |a| a.idle() && a.tabs() == 1);
        s.check("the tree lists the drives under This PC", |a| a.tree_child(a.tree_last_root()) != 0);
        s.check("root tree nodes have an expand arrow", |a| {
            // The arrow sits in the indent left of the icon; TVS_HASBUTTONS gives roots one only with TVS_LINESATROOT.
            let tree = a.tree.hwnd();
            let mut r = RECT::default();
            unsafe { (&raw mut r).cast::<isize>().write_unaligned(a.tree_root()) }; // the whole Bookmarks row
            send(tree, TVM_GETITEMRECT, 0, &mut r as *mut _ as isize);
            let indent = send(tree, TVM_GETINDENT, 0, 0) as i32;
            let mut hit = TVHITTESTINFO { pt: POINT { x: r.left + indent / 2, y: (r.top + r.bottom) / 2 }, ..Default::default() };
            send(tree, TVM_HITTEST, 0, &mut hit as *mut _ as isize);
            hit.flags.0 & TVHT_ONITEMBUTTON.0 != 0
        });
        s.add(
            "expanding a tree folder lists its subfolders",
            |a| {
                send(a.tree.hwnd(), TVM_EXPAND, TVE_EXPAND.0 as usize, a.tree_next(a.tree_root())); // Desktop (after Bookmarks)
            },
            |a| {
                let desktop = known_folder(&FOLDERID_Desktop).map(Location::dir).unwrap_or(Location::ThisPc);
                let expected = list_folder(&desktop, false, None).items.iter().filter(|i| i.is_dir()).count();
                let root = a.tree_next(a.tree_root()); // Desktop
                let mut shown = 0;
                let mut child = a.tree_child(root);
                while child != 0 {
                    shown += 1;
                    child = a.tree_next(child);
                }
                // Done when the subfolders are shown, or the node knows it has none.
                if expected > 0 { shown == expected } else { a.tree_item(root, TVIF_CHILDREN).cChildren.0 == 0 }
            },
        );
        s.add(
            "the user folders arrive from a worker, between Bookmarks and This PC",
            |a| {
                a.tree.fill(); // as at startup and on a drive change
                // Nothing asks Windows about the user folders here (4.1): they arrive by message, later.
                FLAG.set(a.node_texts(a.tree_root()) == "Bookmarks,This PC");
            },
            |a| {
                let mut sfi = SHFILEINFOW::default();
                if let Some(desktop) = known_folder(&FOLDERID_Desktop) {
                    unsafe {
                        SHGetFileInfoW(pcwstr(&wide(desktop)), FILE_FLAGS_AND_ATTRIBUTES(0), Some(&mut sfi), size_of::<SHFILEINFOW>() as u32, SHGFI_DISPLAYNAME);
                    }
                }
                let roots = a.node_texts(a.tree_root());
                let desktop = from_wide(&sfi.szDisplayName).to_string_lossy().into_owned();
                FLAG.get() && roots.starts_with(&format!("Bookmarks,{desktop},")) && roots.ends_with(",This PC")
            },
        );
        s.add(
            "an empty Bookmarks node keeps its arrow and lists a bookmark added later",
            |a| {
                let _ = std::fs::create_dir(p("Bookmarks\\one")); // the startup read found Bookmarks empty
                a.tree_select(a.tree_root());
                a.tree_key(VK_RIGHT); // expand, as the user would
            },
            |a| a.bookmark_nodes() == "one",
        );
        s.add(
            "Bookmarks shows its folders and shortcuts in the tree, open from the start",
            |a| {
                let _ = std::fs::create_dir(p("Bookmarks\\one"));
                make_shortcut(&p("Bookmarks\\sub.lnk"), p("sub"));
                a.tree.fill(); // as at startup
            },
            |a| {
                if a.bookmark_nodes() != "one,sub" {
                    return false;
                }
                // The shortcut's own icon arrives after its node (it may wait on the target).
                let mut own = SHFILEINFOW::default();
                unsafe {
                    SHGetFileInfoW(pcwstr(&wide(p("Bookmarks\\sub.lnk"))), FILE_FLAGS_AND_ATTRIBUTES(0), Some(&mut own), size_of::<SHFILEINFOW>() as u32, SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
                }
                let sub = a.tree_next(a.tree_child(a.tree_root()));
                a.tree_item(sub, TVIF_IMAGE).iImage == own.iIcon
            },
        );
        s.add(
            "choosing a bookmark shortcut in the tree opens its folder",
            |a| {
                a.tree_select(a.tree_child(a.tree_root())); // "one"
                a.tree_key(VK_DOWN); // to "sub", as the user would
            },
            |a| a.at(&p("sub")),
        );
        s.add(
            "Rename on a tree folder's menu opens the rename box for it in the list",
            |a| a.rename_node(Location::dir(p("sub"))),
            move |a| a.at(&dir()) && a.list.renaming() && a.focused_is("sub"),
        );
        s.add(
            "collapsing and expanding a tree folder shows its new subfolders",
            |a| {
                a.cancel_rename(); // the box from the step before
                let _ = std::fs::create_dir(p("Bookmarks\\two"));
                a.tree_select(a.tree_root());
                a.tree_key(VK_LEFT); // collapse, as the user would
                a.tree_key(VK_RIGHT); // and expand again
            },
            |a| a.bookmark_nodes() == "one,two,sub",
        );
        s.add(
            "collapsing a node while a child is being read drops the late result",
            |a| {
                let _ = std::fs::create_dir(p("Bookmarks\\one\\deep"));
                send(a.tree.hwnd(), TVM_EXPAND, TVE_EXPAND.0 as usize, a.tree_child(a.tree_root())); // "one": its read starts
                a.tree_select(a.tree_root());
                a.tree_key(VK_LEFT); // collapse Bookmarks: deletes "one" before its read lands
                a.tree_key(VK_RIGHT); // and read it fresh
            },
            |a| {
                // Both reads landed: no late "deep" under Bookmarks, and every node the tree counts is reachable.
                if step_ms() < 400 || a.bookmark_nodes() != "one,two,sub" {
                    return false;
                }
                let mut node = a.tree_child(a.tree_root());
                while node != 0 {
                    if a.tree_child(node) != 0 {
                        return false;
                    }
                    node = a.tree_next(node);
                }
                a.count_nodes(a.tree_root()) == send(a.tree.hwnd(), TVM_GETCOUNT, 0, 0) as usize
            },
        );
        s.add(
            "(expand a bookmark folder in the tree)",
            |a| {
                send(a.tree.hwnd(), TVM_EXPAND, TVE_EXPAND.0 as usize, a.tree_child(a.tree_root())); // "one"
            },
            |a| a.tree_child(a.tree_child(a.tree_root())) != 0, // "deep" is shown
        );
        s.add(
            "a drive coming or going re-reads This PC and leaves the rest of the tree as it was (4.5)",
            |a| volume_change(a, DBT_DEVICEARRIVAL),
            |a| {
                let one = a.tree_child(a.tree_root());
                let expanded = send(a.tree.hwnd(), TVM_GETITEMSTATE, one as usize, TVIS_EXPANDED.0 as isize) & TVIS_EXPANDED.0 as isize != 0;
                a.bookmark_nodes() == "one,two,sub" && one != 0 && a.tree_child(one) != 0 && expanded && a.tree_child(a.tree_last_root()) != 0
            },
        );
        s.add(
            "This PC in the tree takes no drop",
            |a| {
                let data = data_object_at(a.wnd, &Location::dir(p("file1.txt")));
                let mut effect = DROPEFFECT_COPY;
                if let (Some(data), Some(target)) = (data, a.tree_drop.borrow().clone()) {
                    unsafe {
                        let _ = target.DragEnter(&data, MK_LBUTTON, a.node_point(a.tree_last_root()), &mut effect);
                        let _ = target.DragLeave();
                    }
                }
                EFFECT.set(effect.0);
            },
            |a| EFFECT.get() == DROPEFFECT_NONE.0 && a.drop_highlight() == 0,
        );
        s.add(
            "a drag over a folder in the tree lights it up, and the folder takes the file",
            |a| {
                let _ = std::fs::create_dir(p("Bookmarks\\two\\inner"));
                let data = data_object_at(a.wnd, &Location::dir(p("file1.txt")));
                let mut effect = DROPEFFECT_COPY;
                if let (Some(data), Some(target)) = (data.clone(), a.tree_drop.borrow().clone()) {
                    unsafe {
                        let _ = target.DragEnter(&data, MK_LBUTTON, a.node_point(a.tree_two()), &mut effect);
                    }
                }
                DRAGGED.set(data);
                EFFECT.set(effect.0);
            },
            |a| EFFECT.get() == DROPEFFECT_COPY.0 && a.drop_highlight() == a.tree_two(),
        );
        s.check("held over the closed folder, the drag opens it after a moment", |a| {
            let mut effect = DROPEFFECT_COPY; // Windows asks again many times a second while the drag is held there
            if let Some(target) = a.tree_drop.borrow().clone() {
                unsafe {
                    let _ = target.DragOver(MK_LBUTTON, a.node_point(a.tree_two()), &mut effect);
                }
            }
            a.tree_child(a.tree_two()) != 0 // "inner" is shown
        });
        s.add(
            "dropping it there copies the file into that folder",
            |a| {
                let mut effect = DROPEFFECT_COPY;
                if let (Some(data), Some(target)) = (DRAGGED.take(), a.tree_drop.borrow().clone()) {
                    unsafe {
                        let _ = target.Drop(&data, MK_LBUTTON, a.node_point(a.tree_two()), &mut effect);
                    }
                }
            },
            |a| exists(&p("Bookmarks\\two\\file1.txt")) && a.drop_highlight() == 0,
        )
        .wait(10000);
        s.add(
            "a bookmark shortcut to a folder on this PC takes a drop into that folder",
            |a| {
                let shortcut = a.node_point(a.tree_next(a.tree_two())); // "sub", to the folder sub
                let data = data_object_at(a.wnd, &Location::dir(p("renamed.txt"))); // not in sub yet: Windows would ask
                let mut effect = DROPEFFECT_COPY;
                if let (Some(data), Some(target)) = (data, a.tree_drop.borrow().clone()) {
                    unsafe {
                        let _ = target.DragEnter(&data, MK_LBUTTON, shortcut, &mut effect);
                        EFFECT.set(effect.0);
                        effect = DROPEFFECT_COPY;
                        let _ = target.Drop(&data, MK_LBUTTON, shortcut, &mut effect);
                    }
                }
            },
            |_| EFFECT.get() == DROPEFFECT_COPY.0 && exists(&p("sub\\renamed.txt")),
        )
        .wait(10000);
        s.add(
            "expanding a folder with 10,000 subfolders in the tree keeps the window responsive", // WinSxS has 20,000
            |a| {
                for i in 0..10000 {
                    let _ = std::fs::create_dir(p(&format!("Bookmarks\\two\\inner\\{i}")));
                }
                send(a.tree.hwnd(), TVM_EXPAND, TVE_EXPAND.0 as usize, a.tree_child(a.tree_two())); // "inner"
                T0.set(Some(Instant::now())); // the last timer tick
                KEY_MS.set(0); // the longest the window went without one
            },
            |a| {
                KEY_MS.set(KEY_MS.get().max(T0.get().map_or(0, |t| t.elapsed().as_millis())));
                T0.set(Some(Instant::now()));
                let mut shown = 0;
                let mut child = a.tree_child(a.tree_child(a.tree_two()));
                while child != 0 {
                    shown += 1;
                    child = a.tree_next(child);
                }
                note(format!("{shown} shown, the window waited up to {} ms", KEY_MS.get()));
                shown == 10000 && KEY_MS.get() < 1000
            },
        )
        .wait(10000);
        s.home("(back to the test folder)");

        // Saved tabs: Ctrl+S writes a shortcut per tab in Bookmarks\Tabs, Ctrl+O opens their folders as tabs.
        s.add(
            "Ctrl+S saves each tab as a shortcut in Bookmarks\\Tabs, in tab order, replacing the old ones",
            |a| {
                let _ = std::fs::create_dir(p("Bookmarks\\Tabs"));
                make_shortcut(&p("Bookmarks\\Tabs\\old.lnk"), p("sub")); // an earlier save: replaced
                write_file(&p("Bookmarks\\Tabs\\keep.txt")); // not a shortcut: kept
                a.new_tab(Location::dir(p("sub")), false);
                a.new_tab(Location::ThisPc, false); // tabs: the test folder, sub, This PC
                a.press(letter(b'S'), CTRL);
            },
            |a| {
                let mut saved = list_folder(&Location::dir(p("Bookmarks\\Tabs")), true, None);
                sort_items(&mut saved.items, SortKey::Name, true);
                let clean = |l: Location| match l {
                    Location::Dir(d) => normalize(d.as_os_str()),
                    this_pc => this_pc,
                };
                saved.items.len() == a.tabs() + 1
                    && saved.items.last().is_some_and(|it| it.name == "keep.txt")
                    && (0..a.tabs()).all(|i| {
                        shortcut_target(&p("Bookmarks\\Tabs").join(&saved.items[i].name))
                            .is_some_and(|(target, folder)| folder && clean(target).same(&a.tab_path(i)))
                    })
            },
        );
        s.add(
            "Ctrl+O opens the saved tabs after the open ones, skips any already open, and shows the first new one",
            |a| {
                a.close_tab(2);
                a.close_tab(1);
                a.press(letter(b'O'), CTRL);
            },
            |a| {
                a.tabs() == 3
                    && a.tab_path(1).same(&Location::dir(p("sub")))
                    && matches!(a.tab_path(2), Location::ThisPc)
                    && a.at(&p("sub"))
            },
        );
        s.add(
            "(close those tabs and remove the saved ones)",
            |a| {
                a.close_tab(2);
                a.close_tab(1);
                let _ = std::fs::remove_dir_all(p("Bookmarks\\Tabs"));
            },
            move |a| a.tabs() == 1 && a.at(&dir()),
        );

        // Speed: a folder with 10,000 files.
        s.add(
            "loads a 10,000-file folder",
            |a| {
                let _ = std::fs::create_dir(p("big"));
                for i in 0..10000 {
                    write_file(&p(&format!("big\\item {i}.dat")));
                }
                T0.set(Some(Instant::now()));
                a.open(Location::dir(p("big")), Nav::New, ViewState::default());
            },
            |a| {
                if !a.at(&p("big")) || a.list.count() != 10000 {
                    return false;
                }
                note(format!("load {} ms", T0.get().map_or(0, |t| t.elapsed().as_millis())));
                true
            },
        )
        .wait(30000);
        s.add(
            "filters 10,000 files, showing the matches from the top even when scrolled to the bottom",
            |a| {
                a.list.ensure_visible(Some(9999));
                let t = Instant::now();
                set_text(a.filter, "99");
                note(format!("filter {} ms", t.elapsed().as_millis()));
            },
            |a| a.list.count() == (0..10000).filter(|i: &i32| i.to_string().contains("99")).count() && a.list.top_row() == 0,
        );
        s.add(
            "a run of stars in the filter does not freeze the window (\"********x\" means \"*x\")",
            |a| {
                let t = Instant::now();
                set_text(a.filter, "********x"); // as when Shift+8 is held a little too long
                KEY_MS.set(t.elapsed().as_millis());
                note(format!("filter {} ms", KEY_MS.get()));
            },
            |a| KEY_MS.get() < 1000 && a.list.count() == 0,
        );
        s.add(
            "sorts 10,000 files by each column",
            |a| {
                set_text(a.filter, "");
                let mut text = String::from("sort");
                for (key, name) in [(SortKey::Name, "name"), (SortKey::Type, "type"), (SortKey::Modified, "date"), (SortKey::Size, "size")] {
                    let t = Instant::now();
                    a.list.set_sort(key, false); // descending: the list is in name order already
                    text += &format!(" {name} {}", t.elapsed().as_millis());
                }
                note(text + " ms");
                a.list.set_sort(SortKey::Name, true);
            },
            |a| a.list.count() == 10000,
        );
        s.add(
            "a refresh that asks for an item brings it on screen (as after a rename or a new folder)",
            |a| {
                a.list.ensure_visible(Some(9999)); // scrolled to the bottom
                a.refresh(ViewState::selecting("item 0.dat")); // the first row
            },
            |a| a.idle() && a.focused_is("item 0.dat") && a.focused_on_screen(),
        );
        s.add(
            "Ctrl+T opens a second tab of the folder; the first keeps its selection and scroll position",
            |a| {
                focus(a.list.hwnd());
                a.select_one("item 5000.dat");
                a.list.scroll_to_row(100); // then scrolled away from it, as with the mouse wheel
                TOP.set(a.list.top_row());
                a.press(letter(b'T'), CTRL);
            },
            |a| a.at(&p("big")) && a.tabs() == 2 && a.active() == 1,
        );
        s.add("Ctrl+Tab back restores the selection and the scroll position (4.6)", |a| a.press(VK_TAB, CTRL), |a| {
            a.idle()
                && a.active() == 0
                && a.focused_is("item 5000.dat")
                && a.list.selected_names() == ["item 5000.dat"]
                && a.list.top_row() == TOP.get()
        });
        s.add("(select an item in the second tab)", |a| a.switch_tab(1), |a| {
            if !a.at(&p("big")) || a.active() != 1 {
                return false;
            }
            a.select_one("item 7000.dat");
            true
        });
        s.add(
            "switching away from a tab before it has loaded keeps what both tabs remembered (4.6)",
            |a| {
                // Each switch leaves a tab before its folder loads: what it showed before must survive.
                a.switch_tab(0);
                a.switch_tab(1);
                a.switch_tab(0);
            },
            |a| a.idle() && a.active() == 0 && a.focused_is("item 5000.dat") && a.list.top_row() == TOP.get(),
        )
        .wait(10000); // three loads of 10,000 files: not a false alarm when something else keeps the machine busy
        s.add("(and the second tab keeps its selection too)", |a| a.switch_tab(1), |a| {
            a.idle() && a.active() == 1 && a.focused_is("item 7000.dat")
        });
        s.add("(back to the first tab)", |a| a.switch_tab(0), |a| a.idle() && a.active() == 0);
        s.add(
            "30 quick Ctrl+Tab presses read the folder to the end once, not 30 times (4.2: a new load replaces the old)",
            |a| {
                LOADS.set((a.loads_done.get(), a.loads_wasted.get()));
                focus(a.list.hwnd());
                for _ in 0..30 {
                    a.press(VK_TAB, CTRL);
                }
            },
            |a| {
                let (done, wasted) = LOADS.get();
                let (done, wasted) = (a.loads_done.get() - done, a.loads_wasted.get() - wasted);
                note(format!("{wasted} of {done} reads ran to the end for nothing"));
                a.idle() && a.active() == 0 && done == 30 && wasted <= 3
            },
        )
        .wait(30000);
        s.add(
            "(filter the first tab down to its selected file, then switch tabs)",
            |a| {
                focus(a.list.hwnd());
                a.select_one("item 5000.dat");
                a.type_text("5000");
                a.switch_tab(1);
            },
            |a| a.idle() && a.active() == 1,
        );
        s.add("coming back to a tab that was filtered shows its selected file (4.6)", |a| a.switch_tab(0), |a| {
            a.idle() && a.active() == 0 && a.focused_is("item 5000.dat") && a.focused_on_screen()
        });
        s.add("(close the other tab)", |a| a.close_tab(1), |a| a.idle() && a.tabs() == 1);
        s.add(
            "when another program removes the focused item, the focus moves to the item that took its place (4.5)",
            |a| {
                focus(a.list.hwnd());
                a.select_one("item 5000.dat");
                a.list.ensure_visible(a.list.focused());
                TOP.set(a.list.top_row());
                let _ = std::fs::remove_file(p("big\\item 5000.dat"));
                a.press(VK_F5, 0); // stands in for the change notification
            },
            |a| a.idle() && !a.listed("item 5000.dat") && a.focused_is("item 5001.dat") && a.list.top_row() == TOP.get(),
        );
        s.add(
            "(put that file back)",
            |a| {
                write_file(&p("big\\item 5000.dat"));
                a.refresh(ViewState::default());
            },
            |a| a.idle() && a.listed("item 5000.dat"),
        );
        s.add(
            "(scroll down, minimize the window, and let another program add a file)",
            |a| {
                a.list.scroll_to_row(5000);
                TOP.set(a.list.top_row());
                unsafe {
                    let _ = ShowWindow(a.wnd, SW_SHOWMINNOACTIVE);
                }
                write_file(&p("big\\zz minimized.dat")); // the folder watch refreshes the list
            },
            |a| a.idle() && a.listed("zz minimized.dat"),
        );
        s.add(
            "a folder that changed while the window was minimized keeps its scroll position (4.5)",
            |a| unsafe {
                let _ = ShowWindow(a.wnd, SW_SHOWNOACTIVATE);
            },
            |a| {
                note(format!("top row {}, was {}", a.list.top_row(), TOP.get()));
                a.list.top_row() == TOP.get()
            },
        );
        s.add(
            "(remove that file)",
            |a| {
                let _ = std::fs::remove_file(p("big\\zz minimized.dat"));
                a.refresh(ViewState::default());
            },
            |a| a.idle() && !a.listed("zz minimized.dat"),
        );
        // From This PC: from a folder nearby, the old watch would still see the change through a parent's timestamp.
        s.add("(open This PC, so the next folder is not watched yet)", |a| a.open(Location::ThisPc, Nav::New, ViewState::default()), |a| {
            a.at_pc()
        });
        s.add(
            "a file created while the folder is still being read shows up by itself",
            |a| {
                a.open(Location::dir(p("big")), Nav::New, ViewState::default()); // 10,000 files: read on a worker thread
                std::thread::sleep(Duration::from_millis(100)); // the read is past the start, where this name sorts
                write_file(&p("big\\aaa late.dat")); // too late for the read, too early for the folder watch
                std::thread::sleep(Duration::from_millis(1500)); // this thread waits: the read ends, and Windows reports the file to no one
            },
            |a| a.at(&p("big")) && a.listed("aaa late.dat"),
        );
        s.add(
            "(remove that file too)",
            |a| {
                let _ = std::fs::remove_file(p("big\\aaa late.dat"));
                a.refresh(ViewState::default());
            },
            |a| a.idle() && !a.listed("aaa late.dat"),
        );

        s.add(
            "copies 10,000 selected files to the clipboard",
            |a| {
                a.list.select_all();
                let t = Instant::now();
                copy_to_clipboard(a.wnd, &a.list.location(), &a.list.selected_items(), false);
                note(format!("copy {} ms", t.elapsed().as_millis()));
            },
            |_| clipboard_files().len() == 10000,
        );
        s.add(
            "copying takes the files from the list, not from the disk",
            |a| {
                show_ghosts(a);
                copy_to_clipboard(a.wnd, &a.list.location(), &a.list.selected_items(), false);
            },
            |_| clipboard_files().len() == 3,
        );
        s.add(
            "a Ctrl+C that cannot reach the clipboard says so, not \"copied\" (another program holds it open)",
            |a| {
                // Another thread holds the clipboard open, as a clipboard manager or a remote desktop can.
                let (held, held_wait) = std::sync::mpsc::channel();
                let (release, release_wait) = std::sync::mpsc::channel::<()>();
                let holder = std::thread::spawn(move || {
                    let open = unsafe { OpenClipboard(None) }.is_ok();
                    let _ = held.send(());
                    if open {
                        let _ = release_wait.recv_timeout(Duration::from_secs(10));
                        unsafe {
                            let _ = CloseClipboard();
                        }
                    }
                });
                let _ = held_wait.recv_timeout(Duration::from_secs(5));
                // Selected again and read at once: a refresh in between would replace the files or clear the message.
                show_ghosts(a);
                a.set_message("");
                focus(a.list.hwnd());
                a.press(letter(b'C'), CTRL);
                note(format!("status: {}", a.status_message()));
                let _ = release.send(());
                let _ = holder.join();
            },
            |_| {
                let status = noted();
                !status.contains("copied") && status.contains("clipboard")
            },
        );

        RUN.set(Some(SelfTest { steps: s.0, index: 0, started: None, busy: false, failures: 0 }));
        self.quiet.set(true); // the steps trigger errors on purpose: no beeps
        // Its own timer callback, so the app's code doesn't mention the self-test (the exe you use has none).
        unsafe {
            SetTimer(Some(self.wnd), TIMER_SELFTEST, 50, Some(tick));
        }
    }

    fn self_test_tick(&self) {
        enum Next {
            Wait,
            Act(Option<Act>),
            Check(Check, u128),
            Finish,
        }
        // Decide under a short borrow, then run the step with nothing borrowed: actions can run modal loops.
        let next = RUN.with_borrow(|run| {
            let t = run.as_ref()?;
            Some(if t.busy {
                Next::Wait
            } else if t.index >= t.steps.len() {
                Next::Finish
            } else if t.started.is_none() {
                let step = &t.steps[t.index];
                if self.is_loading() && !step.while_loading { Next::Wait } else { Next::Act(step.act.clone()) }
            } else {
                let s = &t.steps[t.index];
                Next::Check(s.done.clone(), s.timeout_ms)
            })
        });
        let Some(next) = next else { return };
        match next {
            Next::Wait => {}
            Next::Act(act) => {
                note(String::new());
                with_test(|t| {
                    t.started = Some(Instant::now());
                    t.busy = true;
                });
                if let Some(act) = act {
                    act(self);
                }
                // The wait starts after the act: its own setup (10,000 files) is not part of it.
                with_test(|t| {
                    t.busy = false;
                    t.started = Some(Instant::now());
                });
            }
            Next::Check(done, timeout) => {
                let ok = done(self);
                let elapsed = step_ms();
                if !ok && elapsed < timeout {
                    return;
                }
                let note = noted();
                let name = with_test(|t| {
                    t.failures += usize::from(!ok);
                    t.index += 1;
                    t.started = None;
                    t.steps[t.index - 1].name
                });
                let slow = if elapsed > 500 { format!("   (slow step: {elapsed} ms)") } else { String::new() };
                let note = if note.is_empty() { String::new() } else { format!("   [{note}]") };
                log(&format!("{}{name}{note}{slow}\n", if ok { "ok    " } else { "FAIL  " }));
            }
            Next::Finish => {
                unsafe {
                    let _ = KillTimer(Some(self.wnd), TIMER_SELFTEST);
                }
                let failures = with_test(|t| t.failures);
                log(&if failures > 0 { format!("{failures} step(s) FAILED\n") } else { "all steps passed\n".into() });
                let _ = std::fs::remove_dir_all(test_dir());
                EXIT_CODE.set(if failures > 0 { 1 } else { 0 });
                unsafe {
                    let _ = PostMessageW(Some(self.wnd), WM_CLOSE, WPARAM(0), LPARAM(0));
                }
            }
        }
    }
}

fn with_test<R>(f: impl FnOnce(&mut SelfTest) -> R) -> R {
    RUN.with_borrow_mut(|run| f(run.as_mut().expect("self-test running")))
}

unsafe extern "system" fn tick(h: HWND, _: u32, _: usize, _: u32) {
    // SAFETY: as in App's window procedure: the window's App outlives the window.
    if let Some(app) = unsafe { (GetWindowLongPtrW(h, GWLP_USERDATA) as *const App).as_ref() } {
        app.self_test_tick();
    }
}
