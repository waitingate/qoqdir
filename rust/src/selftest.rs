//! `--selftest`, only in a build with the `selftest` feature: a scripted session in the real app. Keys go
//! straight to `App::on_key`, never to other windows. The steps are the C++ version's, in the same order and
//! with the same names.

use std::cell::{Cell, RefCell};
use std::collections::BTreeSet;
use std::ffi::{OsStr, OsString};
use std::io::Write;
use std::os::windows::ffi::OsStrExt;
use std::path::{Component, Path, PathBuf};
use std::rc::Rc;

use std::sync::atomic::{AtomicIsize, AtomicUsize, Ordering};
use std::sync::{Arc, OnceLock};
use std::time::{Duration, Instant};

use windows::Win32::Foundation::{
    COLORREF, ERROR_BAD_NETPATH, HGLOBAL, HWND, LPARAM, POINT, POINTL, RECT, WPARAM,
};
use windows::Win32::Graphics::Gdi::{
    COLOR_WINDOW, ClientToScreen, GetMonitorInfoW, GetSysColor, GetSysColorBrush, MONITOR_DEFAULTTONEAREST, MONITORINFO,
    MonitorFromWindow, CreateCompatibleBitmap, CreateCompatibleDC, DeleteDC, DeleteObject, FillRect, GetDC, GetPixel,
    HDC, ReleaseDC, SelectObject,
};
use windows::Win32::Storage::FileSystem::{
    DDD_EXACT_MATCH_ON_REMOVE, DDD_NO_BROADCAST_SYSTEM, DDD_REMOVE_DEFINITION, DefineDosDeviceW, FILE_ATTRIBUTE_ARCHIVE,
    FILE_ATTRIBUTE_HIDDEN, FILE_FLAGS_AND_ATTRIBUTES, GetFileAttributesW, GetLogicalDrives, GetLongPathNameW,
    SetFileAttributesW,
};
use windows::Win32::System::Com::{DVASPECT_CONTENT, FORMATETC, IDataObject, TYMED_HGLOBAL};
use windows::Win32::System::DataExchange::{
    CloseClipboard, GetClipboardData, GetOpenClipboardWindow, IsClipboardFormatAvailable, OpenClipboard,
    RegisterClipboardFormatW,
};
use windows::Win32::System::Diagnostics::Debug::{GetErrorMode, SEM_FAILCRITICALERRORS};
use windows::Win32::System::Memory::{GlobalLock, GlobalUnlock};
use windows::Win32::System::Ole::{
    DROPEFFECT_COPY, DROPEFFECT_LINK, DROPEFFECT_MOVE, DROPEFFECT_NONE, OleSetClipboard, ReleaseStgMedium,
};
use windows::Win32::System::SystemServices::{MK_CONTROL, MK_LBUTTON};
use windows::Win32::System::Threading::{
    GR_GDIOBJECTS, GR_USEROBJECTS, GetCurrentProcess, GetGuiResources, GetProcessHandleCount,
};
use windows::Win32::UI::Controls::{
    EM_GETCUEBANNER, LVIR_BOUNDS, LVIS_DROPHILITED, LVM_GETHEADER, LVM_GETITEMSTATE,
    EM_SETSEL, EM_GETSEL, EM_REPLACESEL, HTREEITEM, LVIF_IMAGE, LVIR_LABEL, LVITEMW, LVM_CANCELEDITLABEL, LVM_ENSUREVISIBLE, LVM_GETBKCOLOR, LVM_GETCOUNTPERPAGE,
    LVM_GETTEXTBKCOLOR, TVM_GETBKCOLOR,
    LVM_GETITEMRECT, LVM_GETITEMW, TCIF_TEXT, TCITEMW,
    TCM_GETCURSEL, TCM_GETITEMCOUNT, TCM_GETITEMRECT, TCM_GETITEMW, TCM_SETCURFOCUS, UPDOWN_CLASSW,
    TVE_EXPAND, TVGN_CARET, TVHITTESTINFO, TVHT_ONITEMBUTTON, TVIF_CHILDREN,
    TVIF_IMAGE,
    TVIF_TEXT, TVIS_EXPANDED, TVITEMW, TVM_EXPAND, TVM_GETCOUNT, TVM_GETINDENT, TVM_GETITEMRECT, TVM_GETITEMSTATE,
    TVM_GETITEMW, TVM_HITTEST, TVM_SELECTITEM, TTM_GETTOOLCOUNT,
};
use windows::Win32::UI::Input::KeyboardAndMouse::{
    EnableWindow, GetFocus, IsWindowEnabled, VIRTUAL_KEY, VK_BACK, VK_DELETE, VK_DOWN, VK_ESCAPE, VK_F2, VK_F5,
    VK_LEFT, VK_NEXT, VK_PRIOR, VK_RETURN, VK_RIGHT, VK_TAB,
};
use windows::Win32::System::Variant::VARIANT;
use windows::Win32::UI::Accessibility::{AccessibleObjectFromWindow, IAccessible};
use windows::Win32::UI::Shell::{
    CFSTR_DROPDESCRIPTION, DROPDESCRIPTION, DROPIMAGE_INVALID, DROPIMAGE_NOIMAGE,
    DROPFILES, FOLDERID_Desktop, FOLDERID_Profile, SHGFI_DISPLAYNAME, SHGFI_SMALLICON, SHGFI_SYSICONINDEX,
    StrCmpLogicalW,
};
use windows::Win32::UI::WindowsAndMessaging::{
    BM_CLICK, CHILDID_SELF, EnumWindows, FindWindowExW, HTCLIENT, OBJID_CLIENT, GCLP_HBRBACKGROUND, GWLP_USERDATA, GetClassLongPtrW, GetWindowLongPtrW, GetWindowTextLengthW, IsWindowVisible,
    CreateWindowExW, GetWindowRect, HWND_MESSAGE, WINDOW_EX_STYLE, WINDOW_STYLE,
    MSG, PM_NOREMOVE, PeekMessageW,
    KillTimer, PostMessageW, SW_SHOWMINNOACTIVE, SW_SHOWNOACTIVATE, SetTimer, ShowWindow, WM_CHAR, WM_CLOSE,
    WM_DEVICECHANGE, WM_DPICHANGED, WM_KEYDOWN, WM_LBUTTONDBLCLK, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MBUTTONUP, WM_MOUSEMOVE, WM_MOUSEWHEEL,
    WM_NCHITTEST, WM_SYSKEYDOWN, WM_TIMER, PRF_CLIENT, PRF_ERASEBKGND, WM_PRINTCLIENT,
    UIS_CLEAR, UIS_SET, UISF_HIDEFOCUS, WM_QUERYUISTATE, WM_UPDATEUISTATE,
};

use crate::app::{App, Key, Nav, TIMER_LOADING, TIMER_REFRESH, ViewState};
use crate::folder::{
    Item, Listing, Location, SortKey, command_line_words, exists, folder_written, is_directory, is_shortcut,
    list_folder, parse_command_line, sort_items, system_dir,
};
use crate::shell::{
    Terminal, can_paste, command_program, copy_to_clipboard, data_object_at, file_info, in_background, item_menu_at,
    known_folder, paste_into, pidl_from, save_folder_shortcuts, shortcut_target, terminal_program, type_name,
};
use windows::core::{Interface, w};

use crate::util::{
    client_rect, focus, from_wide, mul_div, packed, pcwstr, scale, send, set_text, unscale, wide, window_rect,
    window_text,
};

type Act = Rc<dyn Fn(&App)>;
type Check = Rc<dyn Fn(&App) -> bool>;

struct Step {
    name: &'static str,
    act: Option<Act>,
    done: Check,
    timeout_ms: u128,
    while_loading: bool, // start without waiting for the current folder load to finish
    copies: bool,        // puts files on the clipboard: done again while they aren't there (self_test_tick)
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

    fn copies(&mut self) -> &mut Step {
        self.copies = true;
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

    /// A step back to the first tab, on the test folder.
    fn first_tab(&mut self) {
        self.add("(back to the first tab)", |a| a.switch_tab(0), |a| a.at(&test_dir()) && a.active() == 0);
    }

    /// A step that leaves the address bar with Esc.
    fn esc_back(&mut self) {
        self.add("(Esc puts the path back and returns to the list)", |a| a.press(VK_ESCAPE, 0), |a| {
            let location = a.location(); // not borrowed across the window call below
            text_of(a.address) == location.text().to_string_lossy() && unsafe { GetFocus() } == a.list.hwnd()
        });
    }

    fn push(&mut self, name: &'static str, act: Option<Act>, done: Check) -> &mut Step {
        self.0.push(Step { name, act, done, timeout_ms: 5000, while_loading: false, copies: false });
        self.0.last_mut().expect("just added")
    }
}

struct SelfTest {
    steps: Vec<Step>,
    index: usize,
    started: Option<Instant>, // set once the step's act has run
    held: Option<Instant>,    // since when only a folder load still out has held the next step back
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
    static CRASH_LOG_LEN: Cell<usize> = const { Cell::new(0) }; // the crash log's length before a stall
    // Before a scaling change: the dots per inch, the row height, the first column's width and the window's place.
    static BEFORE: Cell<(u32, i32, i32, RECT)> = const { Cell::new((0, 0, 0, RECT { left: 0, top: 0, right: 0, bottom: 0 })) };
    static EFFECT: Cell<u32> = const { Cell::new(0) };  // what a simulated drag was allowed to do
    static DRAGGED: RefCell<Option<IDataObject>> = const { RefCell::new(None) }; // what a simulated drag carries
    static COUNT: Cell<u32> = const { Cell::new(0) };   // a load count to compare with
    static LOADS: Cell<(u32, u32)> = const { Cell::new((0, 0)) };
    static FLAG: Cell<bool> = const { Cell::new(false) };
    static CUES: Cell<isize> = const { Cell::new(0) };  // the list's focus frames as they were (WM_QUERYUISTATE)
    static MOVES: Cell<usize> = const { Cell::new(0) }; // how far rounds of folders and tabs have got
    static USAGE: Cell<[u32; 3]> = const { Cell::new([0; 3]) }; // handles, GDI and USER objects held before them
    static NEXT: RefCell<OsString> = const { RefCell::new(OsString::new()) }; // the item after the one Delete removes
    static KEPT: RefCell<OsString> = const { RefCell::new(OsString::new()) }; // selected there while a delete is out
    // What `sub` held before the paste step, and anything the paste brought in that isn't the test's file (a cut made
    // elsewhere just as it started): then the test folder is kept, not deleted (7.2).
    static SUB_BEFORE: RefCell<BTreeSet<OsString>> = const { RefCell::new(BTreeSet::new()) };
    static FOREIGN: RefCell<BTreeSet<OsString>> = const { RefCell::new(BTreeSet::new()) };
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

fn text_of(h: HWND) -> String {
    window_text(h).to_string_lossy().into_owned()
}

/// The windows on the screen (any program's), to see that an action opened none.
fn visible_windows() -> Vec<isize> {
    unsafe extern "system" fn each(h: HWND, p: LPARAM) -> windows::core::BOOL {
        if unsafe { IsWindowVisible(h) }.as_bool() {
            unsafe { &mut *(p.0 as *mut Vec<isize>) }.push(h.0 as isize);
        }
        true.into()
    }
    let mut list = Vec::new();
    let _ = unsafe { EnumWindows(Some(each), LPARAM(&mut list as *mut Vec<isize> as isize)) };
    list
}

/// How long the current step has waited since its act.
fn step_ms() -> u128 {
    RUN.with_borrow(|run| run.as_ref().and_then(|t| t.started).map_or(0, |s| s.elapsed().as_millis()))
}

/// %TEMP%\qoqdir-rs-selftest, with long names: paths Windows hands back (shortcut targets) are never RUNNER~1.
fn test_dir() -> PathBuf {
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
            // The names one after another, read straight: DragQueryFileW(i) starts from the first name each time,
            // which took 9 s for 10,000 (and the window showed "not responding").
            let drop = GlobalLock(global) as *const DROPFILES;
            if !drop.is_null() && (*drop).fWide.as_bool() {
                let mut p = drop.cast::<u8>().add((*drop).pFiles as usize).cast::<u16>();
                while *p != 0 {
                    let len = (0..).take_while(|&i| *p.add(i) != 0).count();
                    files.push(PathBuf::from(from_wide(std::slice::from_raw_parts(p, len))));
                    p = p.add(len + 1);
                }
            }
            let _ = GlobalUnlock(global);
        }
        let _ = CloseClipboard();
    }
    files
}

/// The clipboard's text: empty when it holds none.
fn clipboard_text() -> String {
    const CF_UNICODETEXT: u32 = 13;
    let mut text = String::new();
    unsafe {
        if OpenClipboard(None).is_err() {
            return text;
        }
        if let Ok(data) = GetClipboardData(CF_UNICODETEXT) {
            let global = HGLOBAL(data.0);
            let p = GlobalLock(global) as *const u16;
            if !p.is_null() {
                let len = (0..).take_while(|&i| *p.add(i) != 0).count();
                text = String::from_utf16_lossy(std::slice::from_raw_parts(p, len));
            }
            let _ = GlobalUnlock(global);
        }
        let _ = CloseClipboard();
    }
    text
}

/// Paste only the test's own file: after a failed Ctrl+C, pasting would move files the user cut.
fn holds_test_file() -> bool {
    let files = clipboard_files();
    files.len() == 1 && Location::dir(&files[0]).same(&Location::dir(p("file1.txt")))
}

/// Puts the test's file there again if another program has taken the clipboard since.
fn test_file_on_clipboard(a: &App) -> bool {
    if !holds_test_file() {
        let file = Item { name: "file1.txt".into(), attrs: FILE_ATTRIBUTE_ARCHIVE.0, icon: -1, ..Default::default() };
        copy_to_clipboard(a.wnd, &Location::dir(test_dir()), &[file], false);
    }
    holds_test_file()
}

/// A shortcut to `target`; "" is This PC, which has no path.
fn make_shortcut(lnk: &Path, target: impl AsRef<OsStr>) {
    use windows::Win32::System::Com::{CLSCTX_INPROC_SERVER, CoCreateInstance, IPersistFile};
    use windows::Win32::UI::Shell::{IShellLinkW, ShellLink};
    unsafe {
        let Ok(link) = CoCreateInstance::<_, IShellLinkW>(&ShellLink, None, CLSCTX_INPROC_SERVER) else { return };
        if !target.as_ref().is_empty() {
            let _ = link.SetPath(pcwstr(&wide(target)));
        } else if let Some(pc) = pidl_from(&Location::ThisPc) {
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
    client_rect(a.tab_bar).right
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

    fn tab_path(&self, index: usize) -> Location {
        self.st.borrow().tabs.get(index).map_or(Location::ThisPc, |t| t.location.clone())
    }

    fn unavailable(&self) -> bool {
        self.with_tab(|t| t.unavailable)
    }

    fn at(&self, dir: &Path) -> bool {
        self.idle() && self.location().same(&Location::dir(dir))
    }

    fn at_pc(&self) -> bool {
        self.idle() && matches!(self.location(), Location::ThisPc)
    }

    /// The row an item is in.
    fn row_of(&self, name: &str) -> Option<usize> {
        (0..self.list.count()).find(|&r| self.list.item(r).is_some_and(|i| i.name == name))
    }

    fn listed(&self, name: &str) -> bool {
        self.row_of(name).is_some()
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

    fn edit_text(&self) -> String {
        self.list.edit_control().map(text_of).unwrap_or_default()
    }

    fn cancel_rename(&self) {
        send(self.list.hwnd(), LVM_CANCELEDITLABEL, 0, 0);
    }

    /// The middle of a tab, in screen coordinates, where a drag is aimed.
    fn tab_point(&self, index: usize) -> POINTL {
        let r = tab_rect(self, index);
        let mut pt = POINT { x: (r.left + r.right) / 2, y: (r.top + r.bottom) / 2 };
        unsafe {
            let _ = ClientToScreen(self.tab_bar, &mut pt);
        }
        POINTL { x: pt.x, y: pt.y }
    }

    /// The middle of a row's name, in screen coordinates, where a drag is aimed.
    fn row_point(&self, name: &str) -> POINTL {
        let Some(row) = self.row_of(name) else { return POINTL { x: -1, y: -1 } };
        let mut r = RECT { left: LVIR_LABEL as i32, ..Default::default() };
        send(self.list.hwnd(), LVM_GETITEMRECT, row, &mut r as *mut _ as isize);
        let mut pt = POINT { x: (r.left + r.right) / 2, y: (r.top + r.bottom) / 2 };
        unsafe {
            let _ = ClientToScreen(self.list.hwnd(), &mut pt);
        }
        POINTL { x: pt.x, y: pt.y }
    }

    /// A row lit up as a drop's target (5.8).
    fn row_lit(&self, name: &str) -> bool {
        let state = |row| send(self.list.hwnd(), LVM_GETITEMSTATE, row, LVIS_DROPHILITED.0 as isize);
        self.row_of(name).is_some_and(|row| state(row) != 0)
    }

    /// The tip a drag's data carries for its image, "" for none (5.8). An icon beside it shows as the number Windows
    /// gives it, "(1) Copy to %1 [sub]"; a tip with no icon has none.
    fn drag_tip(data: &IDataObject) -> String {
        let fmt = FORMATETC {
            cfFormat: unsafe { RegisterClipboardFormatW(CFSTR_DROPDESCRIPTION) } as u16,
            ptd: std::ptr::null_mut(),
            dwAspect: DVASPECT_CONTENT.0,
            lindex: -1,
            tymed: TYMED_HGLOBAL.0 as u32,
        };
        unsafe {
            let Ok(mut med) = data.GetData(&fmt) else { return String::new() };
            let p = GlobalLock(med.u.hGlobal) as *const DROPDESCRIPTION;
            let mut tip = String::new();
            if !p.is_null() {
                // Copied out whole: the struct is packed, so its fields can't be borrowed where they are.
                let d = std::ptr::read_unaligned(p);
                let (kind, text, name) = (d.r#type, d.szMessage, d.szInsert);
                if kind != DROPIMAGE_INVALID && kind != DROPIMAGE_NOIMAGE {
                    tip = format!("({}) ", kind.0);
                }
                if kind != DROPIMAGE_INVALID {
                    tip += &format!("{} [{}]", from_wide(&text).to_string_lossy(), from_wide(&name).to_string_lossy());
                }
                let _ = GlobalUnlock(med.u.hGlobal);
            }
            ReleaseStgMedium(&mut med);
            tip
        }
    }

    /// Bookmarks' folder "one", the first.
    fn tree_one(&self) -> isize {
        self.tree.child(self.tree.root())
    }

    /// Bookmarks' folder "two".
    fn tree_two(&self) -> isize {
        self.tree.next_sibling(self.tree_one())
    }

    /// Where a tree node is in the tree: its text, or its whole row.
    fn node_rect(&self, node: isize, text_only: bool) -> RECT {
        let mut r = RECT::default();
        unsafe { (&raw mut r).cast::<isize>().write_unaligned(node) }; // TVM_GETITEMRECT takes the item in the rect
        send(self.tree.hwnd(), TVM_GETITEMRECT, usize::from(text_only), &mut r as *mut _ as isize);
        r
    }

    /// The middle of a tree node, in screen coordinates, where a drag is aimed.
    fn node_point(&self, node: isize) -> POINTL {
        let r = self.node_rect(node, true);
        let mut pt = POINT { x: (r.left + r.right) / 2, y: (r.top + r.bottom) / 2 };
        unsafe {
            let _ = ClientToScreen(self.tree.hwnd(), &mut pt);
        }
        POINTL { x: pt.x, y: pt.y }
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

    fn expanded(&self, node: isize) -> bool {
        send(self.tree.hwnd(), TVM_GETITEMSTATE, node as usize, TVIS_EXPANDED.0 as isize) & TVIS_EXPANDED.0 as isize != 0
    }

    fn node_text(&self, node: isize) -> String {
        let mut text = [0u16; 64];
        let mut item = TVITEMW {
            mask: TVIF_TEXT,
            hItem: HTREEITEM(node),
            pszText: windows::core::PWSTR(text.as_mut_ptr()),
            cchTextMax: 64,
            ..Default::default()
        };
        send(self.tree.hwnd(), TVM_GETITEMW, 0, &mut item as *mut _ as isize);
        from_wide(&text).to_string_lossy().into_owned()
    }

    /// The list drawn into a bitmap, as the filter box is: which rows have a line under them, and whether one shows
    /// below the last row, where the next would be (5.2).
    fn row_lines(&self) -> (Vec<bool>, bool) {
        /// How much of pixel row `y` of `dc`, from `from` to `to`, has the colour `line`.
        fn share(dc: HDC, line: COLORREF, y: i32, from: i32, to: i32) -> f64 {
            let n = (from..to).filter(|&x| unsafe { GetPixel(dc, x, y) } == line).count();
            if to > from { n as f64 / f64::from(to - from) } else { 0.0 }
        }
        let lv = self.list.hwnd();
        let Some(line) = self.list.line_color() else { return (Vec::new(), true) };
        let rc = client_rect(lv);
        let (mut lined, mut last) = (Vec::new(), RECT::default());
        unsafe {
            let screen = GetDC(Some(lv));
            let dc = CreateCompatibleDC(Some(screen));
            let bitmap = CreateCompatibleBitmap(screen, rc.right, rc.bottom);
            let old = SelectObject(dc, bitmap.into());
            send(lv, WM_PRINTCLIENT, dc.0 as usize, (PRF_CLIENT | PRF_ERASEBKGND) as isize);
            for row in 0..self.list.count() {
                let mut r = RECT { left: LVIR_BOUNDS as i32, ..Default::default() };
                if send(lv, LVM_GETITEMRECT, row, &mut r as *mut _ as isize) == 0 || r.bottom > rc.bottom {
                    break;
                }
                lined.push(share(dc, line, r.bottom - 1, r.left.max(0), r.right.min(rc.right)) > 0.9);
                last = r;
            }
            let under = last.bottom + (last.bottom - last.top) - 1; // where a line under one more row would be
            let below = under < rc.bottom && share(dc, line, under, 0, rc.right) > 0.0;
            SelectObject(dc, old);
            let _ = DeleteObject(bitmap.into());
            let _ = DeleteDC(dc);
            ReleaseDC(Some(lv), screen);
            (lined, below)
        }
    }

    /// The filter box drawn into a bitmap: whether its right third has anything on it but the box's own paper, as the
    /// funnel the empty box shows does (5.1). A bitmap, so a window over this one on screen makes no difference.
    fn funnel_shown(&self) -> bool {
        unsafe {
            let rc = client_rect(self.filter);
            let screen = GetDC(Some(self.filter));
            let dc = CreateCompatibleDC(Some(screen));
            let bitmap = CreateCompatibleBitmap(screen, rc.right, rc.bottom);
            let old = SelectObject(dc, bitmap.into());
            FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
            send(self.filter, WM_PRINTCLIENT, dc.0 as usize, PRF_CLIENT as isize);
            let paper = GetSysColor(COLOR_WINDOW);
            let mut marks = 0;
            for x in rc.right * 2 / 3..rc.right {
                for y in 0..rc.bottom {
                    marks += usize::from(GetPixel(dc, x, y).0 != paper);
                }
            }
            SelectObject(dc, old);
            let _ = DeleteObject(bitmap.into());
            let _ = DeleteDC(dc);
            ReleaseDC(Some(self.filter), screen);
            marks > 0
        }
    }

    /// The texts of a node and its next siblings, comma-separated.
    fn node_texts(&self, first: isize) -> String {
        self.tree.siblings(first).map(|n| self.node_text(n)).collect::<Vec<_>>().join(",")
    }

    /// The texts under Bookmarks, the tree's first root.
    fn bookmark_nodes(&self) -> String {
        self.node_texts(self.tree_one())
    }

    /// The nodes from `first` on, and all below them.
    fn count_nodes(&self, first: isize) -> usize {
        self.tree.siblings(first).map(|n| 1 + self.count_nodes(self.tree.child(n))).sum()
    }

    pub fn start_self_test(&self) {
        let _ = std::fs::write(log_path(), ""); // the log starts empty; every step then appends its line
        crate::crashlog::log_in(&test_dir()); // a crash or hang during the test is logged beside its own log
        let dir = test_dir;
        let mut s = Steps::default();

        // 6 items: the 5 test files and folders, plus the self-test's own Bookmarks folder.
        s.check("opens the start folder with all 6 items", move |a| a.at(&dir()) && a.list.count() == 6);
        s.check("at start, the status bar says F1 shows the keys (5.10)", |a| {
            a.status_message() == "F1 shows the keys"
        });
        s.check("shows a Chinese file name", |a| a.listed("\u{5831}\u{544A}.pdf"));
        s.check("the folder Qoqdir was started from is not kept open (its working folder is the system folder)", |_| {
            let cwd = std::env::current_dir().unwrap_or_default();
            note(format!("working folder {}", cwd.display()));
            Location::dir(cwd).same(&Location::dir(system_dir().unwrap_or_default()))
        });
        s.check("a drive without a disk fails quietly, without Windows' \"No Disk\" box", |_| {
            let mode = unsafe { GetErrorMode() };
            mode & SEM_FAILCRITICALERRORS.0 != 0
        });
        // Screen readers read a button's accessible name, and the filter box's; a tooltip shows it to everyone else
        // (5.1).
        let name = "the toolbar buttons and the filter box say what they do and their keys, to screen readers and in \
                    tooltips";
        s.check(name, |a| {
            let named = [
                (a.back, "Back (Alt+Left)"),
                (a.forward, "Forward (Alt+Right)"),
                (a.up, "Up (Backspace)"),
                (a.refresh, "Refresh (F5)"),
                (a.filter, "Filter (Ctrl+F)"),
            ];
            let name_of = |h: HWND| unsafe {
                let mut acc: Option<IAccessible> = None;
                AccessibleObjectFromWindow(h, OBJID_CLIENT.0 as u32, &IAccessible::IID, &mut acc as *mut _ as *mut _).ok()?;
                acc?.get_accName(&VARIANT::from(CHILDID_SELF as i32)).ok().map(|n| n.to_string())
            };
            let all_named = named.iter().all(|&(h, text)| name_of(h).as_deref() == Some(text));
            all_named && send(a.tips, TTM_GETTOOLCOUNT, 0, 0) == 5
        });
        // The empty filter box draws a pale funnel at its right end instead of a word, and text in it hides the funnel
        // (5.1).
        s.check("the empty filter box shows a small funnel at its right end, not a word (5.1)", |a| {
            let mut cue = [0u16; 8];
            let asked = send(a.filter, EM_GETCUEBANNER, cue.as_mut_ptr() as usize, cue.len() as isize);
            let word = asked != 0 && cue[0] != 0;
            !word && a.funnel_shown()
        });
        s.add("(text in it hides the funnel)", |a| set_text(a.filter, "zzz"), |a| {
            a.list.count() == 0 && !a.funnel_shown()
        });
        s.add("(empty again, it shows the funnel)", |a| set_text(a.filter, ""), |a| {
            a.list.count() == 6 && a.funnel_shown()
        });
        s.add(
            "a pale line runs under each row of the list, not under a selected one, and none below the last row (5.2)",
            |a| {
                a.select_one("sub");
            },
            |a| {
                let (lined, below) = a.row_lines();
                let sub = a.row_of("sub");
                // Lined rows besides "sub": one may lack it while the mouse is over it (hot).
                let others = lined.iter().enumerate().filter(|&(i, &l)| Some(i) != sub && l).count();
                note(format!("{others} other row(s) lined"));
                lined.len() == 6 && sub.is_some_and(|s| !lined[s]) && others >= 4 && !below
            },
        );
        // Windows hides the focus frame until a key is pressed (the window's UI state says so): the focused row then
        // has no frame, so it keeps its line (5.2).
        s.add(
            "with focus frames hidden, as before a key is pressed, the focused row keeps its line (5.2)",
            |a| {
                focus(a.list.hwnd());
                a.list.select(&[], Some(OsStr::new("sub"))); // focused, not selected
                CUES.set(send(a.list.hwnd(), WM_QUERYUISTATE, 0, 0));
                send(a.list.hwnd(), WM_UPDATEUISTATE, (UIS_SET | (UISF_HIDEFOCUS << 16)) as usize, 0);
            },
            |a| {
                let (lined, _) = a.row_lines();
                a.row_of("sub").is_some_and(|s| lined.get(s) == Some(&true))
            },
        );
        s.add(
            "(and the focus frames as they were)",
            |a| {
                let action = if CUES.get() & UISF_HIDEFOCUS as isize != 0 { UIS_SET } else { UIS_CLEAR };
                send(a.list.hwnd(), WM_UPDATEUISTATE, (action | (UISF_HIDEFOCUS << 16)) as usize, 0);
            },
            |_| true,
        );
        s.add(
            "typing in the list filters it, and the status bar says so the first time",
            |a| {
                focus(a.list.hwnd());
                a.type_text("file");
            },
            |a| {
                a.list.count() == 3
                    && unsafe { GetFocus() } == a.filter
                    && a.status_message() == "Typing filters the list; Esc shows all again"
            },
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
                note(format!("filter \"{}\"", text_of(a.filter)));
                text_of(a.filter) == "\u{5831}\u{544A}" && a.list.count() == 1
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
                note(format!("filter \"{}\"", text_of(a.filter)));
                text_of(a.filter) == "file "
            },
        );
        s.add(
            "Ctrl+Backspace in the address bar deletes the last word too",
            |a| {
                focus(a.address);
                set_text(a.address, "D:\\my files");
                send(a.address, EM_SETSEL, 11, 11); // the caret at the end
                a.press(VK_BACK, CTRL);
            },
            |a| {
                note(format!("address \"{}\"", text_of(a.address)));
                text_of(a.address) == "D:\\my "
            },
        );
        s.esc_back();
        // The tree's width (96-DPI units) before a drag, and what a drag far past the window's edge left it at.
        let drag = Rc::new(Cell::new((0, 0)));
        let (drag_act, drag_done) = (drag.clone(), drag);
        s.add(
            "dragging the splitter past the window's edge keeps the width shown (what is saved), not one past the edge",
            move |a| {
                let rc = client_rect(a.wnd);
                let (y, button) = (rc.bottom / 2, MK_LBUTTON.0 as usize);
                let before = a.tree_width.get();
                send(a.wnd, WM_LBUTTONDOWN, button, packed(a.splitter_x.get() + 1, y));
                send(a.wnd, WM_MOUSEMOVE, button, packed(rc.right + 1000, y));
                drag_act.set((before, a.tree_width.get()));
                let back = scale(before, a.dpi.get());
                send(a.wnd, WM_MOUSEMOVE, button, packed(back, y)); // and back
                send(a.wnd, WM_LBUTTONUP, 0, packed(back, y));
            },
            move |a| {
                let (before, far) = drag_done.get();
                let rc = client_rect(a.wnd);
                let dpi = a.dpi.get();
                let limit = unscale(scale(80, dpi).max(rc.right - scale(200, dpi)), dpi);
                note(format!("far {far}, limit {limit}, back to {}", a.tree_width.get()));
                far <= limit + 1 && (a.tree_width.get() - before).abs() <= 1 && !a.dragging.get()
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
        )
        .copies();
        s.add(
            "filter + Enter opens a folder",
            |a| {
                a.type_text("sub");
                a.press(VK_RETURN, 0);
            },
            |a| a.at(&p("sub")),
        );
        // The names in a folder, hidden ones too.
        fn names_in(dir: &Path) -> BTreeSet<OsString> {
            std::fs::read_dir(dir)
                .map(|d| d.filter_map(|e| e.ok().map(|e| e.file_name())).collect())
                .unwrap_or_default()
        }
        s.add(
            "Ctrl+V pastes with the Windows copy engine",
            |a| {
                SUB_BEFORE.with(|b| *b.borrow_mut() = names_in(&p("sub")));
                if test_file_on_clipboard(a) {
                    a.press(letter(b'V'), CTRL);
                }
            },
            |_| {
                // Only the test's file comes in. A cut made elsewhere just as the paste starts would bring someone's
                // files instead: they are named here, and the test folder is kept at the end (7.2).
                let before = SUB_BEFORE.with(|b| b.borrow().clone());
                let came = names_in(&p("sub"))
                    .into_iter()
                    .filter(|n| n != OsStr::new("file1.txt") && !before.contains(n));
                FOREIGN.with(|f| f.borrow_mut().extend(came));
                let foreign = FOREIGN.with(|f| f.borrow().clone());
                if !foreign.is_empty() {
                    let names: Vec<_> = foreign.iter().map(|n| n.to_string_lossy()).collect();
                    note(format!("not the test's: {}", names.join(", ")));
                    return false;
                }
                exists(&p("sub\\file1.txt"))
            },
        )
        .wait(10000);
        s.check("pasted file appears in the list by itself", |a| a.idle() && a.listed("file1.txt"));
        s.add(
            "Paste shortcut makes a shortcut to the copied file",
            |a| {
                if test_file_on_clipboard(a) {
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
        // Ctrl+Shift+C: the paths as text, each quoted, one per line, as Explorer's Copy as path (5.4).
        fn quoted_paths(a: &App) -> String {
            let dir = a.list.location();
            let names = a.list.selected_names();
            names.iter().map(|n| format!("\"{}\"", dir.join(n).display())).collect::<Vec<_>>().join("\r\n")
        }
        s.add(
            "Ctrl+Shift+C copies the selection's full paths, quoted and one per line, as Explorer's Copy as path",
            |a| {
                focus(a.list.hwnd());
                a.list.select_all();
                a.press(letter(b'C'), CTRL | SHIFT);
            },
            |a| {
                let n = a.list.selected_names().len();
                n >= 2 && clipboard_text() == quoted_paths(a) && a.status_message() == format!("{n} path(s) copied")
            },
        )
        .copies();
        s.add(
            "with nothing selected, it copies the folder's own path",
            |a| {
                a.list.select(&[], None);
                a.press(letter(b'C'), CTRL | SHIFT);
            },
            |a| {
                clipboard_text() == format!("\"{}\"", p("sub").display()) && a.status_message() == "1 path(s) copied"
            },
        )
        .copies();
        s.add(
            "Backspace goes up and selects the folder you came from, and the status bar says so the first time",
            |a| a.press(VK_BACK, 0),
            move |a| {
                a.at(&dir())
                    && a.focused_is("sub")
                    && a.list.selected_names().len() == 1
                    && a.status_message() == "Up (Alt+Left goes back)"
            },
        );
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
            "Rename on the item menu renames the selected item, not one the keyboard left the focus on",
            |a| {
                set_text(a.filter, ""); // the F2 step typed "file2" into it
                focus(a.list.hwnd());
                if let Some(row) = a.select_one("file1.txt") {
                    a.list.focus_row(row + 1); // as Ctrl+Down leaves it: the focus moves, the selection stays
                }
                a.item_verb("rename", &a.menu_items());
            },
            |a| a.list.renaming() && a.edit_text() == "file1.txt",
        );
        s.add("(cancel that rename)", |a| a.cancel_rename(), |a| !a.list.renaming());
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
        // A big delete takes minutes: what it selects afterwards is about its own folder, not one shown since.
        s.add(
            "(a file to delete)",
            |a| {
                write_file(&p("doomed.txt"));
                a.refresh(ViewState::default());
            },
            |a| a.idle() && a.listed("doomed.txt"),
        );
        s.add(
            "(Delete it, and open sub before the delete is done)",
            |a| {
                focus(a.list.hwnd());
                a.select_one("doomed.txt");
                a.press(VK_DELETE, 0);
                a.open(Location::dir(p("sub")), Nav::New, ViewState::default());
                COUNT.set(a.generation.get());
            },
            |a| {
                if !a.at(&p("sub")) || exists(&p("doomed.txt")) {
                    return false;
                }
                let last = a.list.count().checked_sub(1).and_then(|row| a.list.item(row));
                let name = last.map(|it| it.name).unwrap_or_default();
                a.list.select(std::slice::from_ref(&name), Some(name.as_os_str()));
                KEPT.with(|k| *k.borrow_mut() = name);
                true
            },
        )
        .wait(10000);
        s.add(
            "a Delete that is done after the tab went to another folder leaves that folder as it is (4.6)",
            |_| {},
            |a| {
                note(format!("{} read(s)", a.generation.get() - COUNT.get()));
                let kept = KEPT.with(|k| k.borrow().clone());
                let kept: Vec<OsString> = Some(kept).filter(|n| !n.is_empty()).into_iter().collect();
                // 0.8 s of nothing: the delete's reply comes as soon as the file is gone.
                step_ms() >= 800 && a.generation.get() == COUNT.get() && a.list.selected_names() == kept
            },
        );
        s.home("(back to the test folder)");
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
            a.commands_done.get() == a.commands.get()
                && a.at(&p("gone"))
                && a.status_message().contains("not available")
        });
        s.add(
            "F5 tries again once it is back",
            |a| {
                let _ = std::fs::create_dir(p("gone"));
                a.press(VK_F5, 0);
            },
            |a| {
                a.at(&p("gone")) && a.list.shows(&Location::dir(p("gone"))) && !a.status_message().contains("not available")
            },
        );
        s.add(
            "(back up to the test folder)",
            |a| {
                focus(a.list.hwnd());
                a.press(VK_BACK, 0);
            },
            move |a| a.at(&dir()),
        );
        let one_tab_home = move |a: &App| a.tabs() == 1 && a.at(&dir()); // back to the test folder's own tab
        s.add("Ctrl+T opens a tab of the same folder", |a| a.press(letter(b'T'), CTRL), move |a| {
            a.tabs() == 2 && a.at(&dir())
        });
        s.add("Ctrl+W closes it", |a| a.press(letter(b'W'), CTRL), one_tab_home);
        s.add(
            "Ctrl+W on the last tab keeps it (the last tab cannot be closed)",
            |a| a.press(letter(b'W'), CTRL),
            one_tab_home,
        );
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
        s.add("(close that tab)", |a| a.close_tab(1), one_tab_home);
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
            "a touchpad's small steps over the tabs add up to one wheel step",
            |a| {
                for _ in 0..4 {
                    wheel(a, -30); // four small steps make one
                }
            },
            move |a| a.active() == 1 && a.at(&dir()),
        );
        s.add(
            "a wheel step shows the next tab, and past the last it stops",
            |a| {
                wheel(a, -120);
                wheel(a, -120); // past the last tab: nothing
            },
            |a| a.active() == 2 && a.at_pc(),
        );
        s.add(
            "turning the wheel the other way goes back to the first tab, and stops there",
            |a| {
                for _ in 0..3 {
                    wheel(a, 120);
                }
            },
            |a| a.active() == 0 && a.at(&p("sub")),
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
        s.add("(close the other tabs)", |a| a.close_other_tabs(1), one_tab_home);
        s.add(
            "a double-click on a tab opens no tab",
            |a| {
                let r = tab_rect(a, 0);
                let (x, y) = ((r.left + r.right) / 2, (r.top + r.bottom) / 2);
                send(a.tab_bar, WM_LBUTTONDBLCLK, MK_LBUTTON.0 as usize, packed(x, y));
                send(a.tab_bar, WM_LBUTTONUP, 0, packed(x, y));
            },
            move |a| a.tabs() == 1 && a.active() == 0 && a.at(&dir()),
        );
        s.add(
            "a double-click on the empty bar right of the tabs opens one, as \"+\" does",
            |a| {
                let r = tab_rect(a, 0);
                let (x, y) = (r.right + scale(20, a.dpi.get()), (r.top + r.bottom) / 2); // right of the only tab
                send(a.tab_bar, WM_LBUTTONDBLCLK, MK_LBUTTON.0 as usize, packed(x, y));
                send(a.tab_bar, WM_LBUTTONUP, 0, packed(x, y));
            },
            move |a| {
                let r = tab_rect(a, a.tabs() - 1); // a real click lands only where the bar says it is its own: right of the last tab
                let mut pt = POINT { x: r.right + scale(20, a.dpi.get()), y: (r.top + r.bottom) / 2 };
                let _ = unsafe { ClientToScreen(a.tab_bar, &mut pt) };
                let takes_click = send(a.tab_bar, WM_NCHITTEST, 0, packed(pt.x, pt.y)) == HTCLIENT as isize;
                takes_click && a.tabs() == 2 && a.active() == 1 && a.at(&dir())
            },
        );
        s.add("(close it)", |a| a.close_tab(1), one_tab_home);
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
        s.add("(close them)", |a| a.close_other_tabs(a.active()), one_tab_home);
        s.add(
            "a tab the control selects by itself (as assistive tools select one) switches to its folder",
            |a| {
                a.new_tab(Location::dir(p("sub")), false); // a second tab, right of this one
                send(a.tab_bar, TCM_SETCURFOCUS, 1, 0); // on plain tabs this selects, and notifies the window as a click would
            },
            |a| a.active() == 1 && a.at(&p("sub")),
        );
        s.add("(close that tab)", |a| a.close_tab(1), one_tab_home);
        // What Windows sends when the window reaches a monitor with another scaling: the dots per inch there, and where
        // the window goes.
        let dpi_change = |a: &App, dpi: u32, at: RECT| send(a.wnd, WM_DPICHANGED, (dpi | dpi << 16) as usize, &at as *const _ as isize);
        s.add(
            "a move to a monitor with another scaling (as Windows reports it) rescales the font, the columns and the tabs",
            move |a| {
                let at = window_rect(a.wnd);
                BEFORE.set((a.dpi.get(), a.row_height.get(), a.list.column_widths()[0], at));
                let bigger = RECT { right: at.left + (at.right - at.left) * 4 / 3, bottom: at.top + (at.bottom - at.top) * 4 / 3, ..at };
                dpi_change(a, a.dpi.get() * 4 / 3, bigger);
            },
            |a| {
                let (dpi0, row0, col0, _) = BEFORE.get();
                let dpi = dpi0 * 4 / 3;
                a.dpi.get() == dpi
                    && a.row_height.get() > row0
                    && a.list.column_widths()[0] == mul_div(col0, dpi as i32, dpi0 as i32)
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
        // The switch showed where the tab is at once: what is typed into the address bar since is the user's (5.3).
        s.first_tab();
        s.add(
            "a tab switch whose folder then loads leaves what was typed into the address bar meanwhile (5.3)",
            |a| {
                a.switch_tab(1);
                set_text(a.address, "typed meanwhile"); // before the tab's folder can have loaded
            },
            |a| a.at(&p("sub")) && text_of(a.address) == "typed meanwhile",
        );
        s.first_tab();
        s.add(
            "...and so does one whose folder turns out not to be available (4.3)",
            |a| {
                a.switch_tab(1);
                set_text(a.address, "typed meanwhile");
                // The switch's read, as a share that is off answers it in the end.
                let failed = Listing {
                    location: Location::dir(p("sub")),
                    error: Some(ERROR_BAD_NETPATH.to_hresult().into()),
                    ..Default::default()
                };
                a.on_loaded(failed, a.generation.get());
            },
            |a| {
                a.idle()
                    && a.unavailable()
                    && a.status_message().contains("not available")
                    && text_of(a.address) == "typed meanwhile"
            },
        );
        s.add("(close that tab)", |a| a.close_tab(a.active()), one_tab_home);
        s.add(
            "...and so does a move to the folder shown, while the user types there (5.3)",
            |a| {
                // As a click on the tree's selected folder: it is read again.
                a.open(Location::dir(test_dir()), Nav::New, ViewState::default());
                focus(a.address);
                set_text(a.address, "typed meanwhile");
            },
            move |a| a.at(&dir()) && text_of(a.address) == "typed meanwhile",
        );
        s.add(
            "...and a Back to the folder shown still sets the Back and Forward buttons (5.1)",
            move |a| {
                // The history a Back that failed on a deleted folder can leave: the folder shown, once more (4.3).
                a.with_tab(|t| {
                    t.back = vec![Location::dir(dir())];
                    t.forward.clear();
                });
                unsafe {
                    // The buttons as they were for it.
                    let _ = EnableWindow(a.back, true);
                    let _ = EnableWindow(a.forward, false);
                }
                a.press(VK_LEFT, ALT); // in the address bar
            },
            move |a| {
                a.at(&dir())
                    && text_of(a.address) == "typed meanwhile"
                    && !unsafe { IsWindowEnabled(a.back) }.as_bool()
                    && unsafe { IsWindowEnabled(a.forward) }.as_bool()
            },
        );
        s.esc_back();
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
        // The program is looked up on the PATH and started on a worker, so a folder there that doesn't answer
        // (Windows can take 20 to 60 s to give up on one) can't hold the window (4.1). Its reply is a message, so it
        // can't be in yet when the key press returns.
        let answered = Rc::new(Cell::new(false));
        let (answered_act, answered_done) = (answered.clone(), answered);
        s.add(
            "a typed command is looked up and started on a worker, so a folder on the PATH that doesn't answer can't \
             hold the window (4.1)",
            move |a| {
                a.set_message("");
                a.go_to("no-such-program-qoqdir");
                answered_act.set(!a.status_message().is_empty());
            },
            move |a| {
                if answered_done.get() {
                    note("answered before the key press returned".into());
                }
                !answered_done.get() && a.status_message().contains("Can't find")
            },
        );
        s.add(
            "a program typed by name is Windows' own, not a file of that name in the folder shown",
            |a| {
                write_file(&p("rundll32.exe")); // not a program: run from here, it would fail
                a.set_message("");
                a.go_to("rundll32");
            },
            move |a| {
                if a.commands_done.get() != a.commands.get() {
                    return false; // looked up on a worker: the decoy stays until its reply
                }
                let _ = std::fs::remove_file(p("rundll32.exe"));
                note(format!("status: {}", a.status_message()));
                a.at(&dir()) && a.status_message().is_empty()
            },
        );
        let ok = Rc::new(Cell::new(false)); // {a name on neither PATH nor App Paths is refused, "cmd" resolves}
        let (ok_act, ok_done) = (ok.clone(), ok);
        s.add(
            "a program name on neither the PATH nor App Paths is refused, not run from the folder shown",
            move |_a| {
                write_file(&p("qoqnotonpath.exe")); // a decoy in the folder: command_program must ignore it
                let decoy = command_program(OsStr::new("qoqnotonpath"));
                let on_path = command_program(OsStr::new("cmd"));
                let _ = std::fs::remove_file(p("qoqnotonpath.exe"));
                let full = on_path.map(|f| String::from_utf16_lossy(&f).trim_end_matches('\0').to_lowercase());
                note(format!("cmd -> {}", full.clone().unwrap_or_else(|| "(none)".into())));
                let cmd_full = full.is_some_and(|s| s.ends_with("cmd.exe") && s.contains('\\'));
                ok_act.set(decoy.is_none() && cmd_full);
            },
            move |_a| ok_done.get(),
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
            |_| KEY_MS.get() < 100, // 4.1, item 15: well under 100 ms (the read is on a worker)
        );
        s.add(
            "...and opening another folder cancels it",
            move |a| {
                // The network read reports back after its timeout, during a later step.
                a.uncounted_load.set(a.generation.get());
                a.open(Location::dir(dir()), Nav::New, ViewState::default());
            },
            move |a| a.at(&dir()),
        )
        .during_load();
        s.add(
            "typing the path to a folder shortcut opens the folder here, not in Explorer (4.4)",
            |a| {
                make_shortcut(&p("ff-link.lnk"), p("sub"));
                a.go_to(p("ff-link.lnk"));
            },
            |a| a.at(&p("sub")),
        );
        s.add(
            "(remove the shortcut and return to the test folder)",
            move |a| {
                let _ = std::fs::remove_file(p("ff-link.lnk"));
                a.open(Location::dir(dir()), Nav::New, ViewState::default());
            },
            move |a| a.at(&dir()) && !p("ff-link.lnk").exists(),
        );
        s.add("typing a subfolder's name opens it here (not in Explorer)", |a| a.go_to("sub"), |a| a.at(&p("sub")));
        s.add("typing ..\\ goes up", |a| a.go_to("..\\"), move |a| a.at(&dir()));
        s.add("typing .\\sub opens it here (not in Explorer)", |a| a.go_to(".\\sub"), |a| a.at(&p("sub")));
        // A path from the root without a drive ("\Users\...\qoqdir-selftest", as a script or another program can give
        // one): on the folder's own drive or share, as Explorer takes it (5.3).
        s.add(
            "typing a path that starts at the root, without a drive, opens it on the folder's own drive",
            move |a| a.go_to(dir().components().filter(|c| !matches!(c, Component::Prefix(_))).collect::<PathBuf>()),
            move |a| a.at(&dir()),
        );
        s.add(
            "a long folder name of emojis is cut to fit its tab, ending in an ellipsis, with no broken character",
            |a| {
                let name = "\u{1F600}".repeat(40); // each is two UTF-16 units; the cut lands among them
                let _ = std::fs::create_dir(p(&name));
                a.open(Location::dir(p(&name)), Nav::New, ViewState::default());
            },
            |a| {
                if !a.idle() || a.location().display_name().encode_wide().count() != 80 {
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
                note(format!("tab at {}, status: {}", a.location().text().to_string_lossy(), a.status_message()));
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
                note(format!("tab at {}, {} tab(s)", a.location().text().to_string_lossy(), a.tabs()));
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
                note(format!("filter \"{}\"", text_of(a.filter)));
                a.at(&dir()) && text_of(a.filter) == "file"
            },
        );
        s.add(
            "a folder that fails to open leaves the filter of the folder still shown",
            |a| a.open(Location::dir(p("no-such-folder")), Nav::New, ViewState::default()),
            move |a| {
                note(format!("filter \"{}\"", text_of(a.filter)));
                a.at(&dir()) && text_of(a.filter) == "file"
            },
        );
        s.add(
            "a refresh queued just before a tab became not available waits for F5 (4.3)",
            move |a| {
                a.queue_refresh(); // a change in the folder, just before
                // Then it became unreachable.
                a.show_unavailable(&Location::dir(dir()), "The network path was not found.");
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
                let last_back = a.with_tab(|t| t.back.last().cloned());
                a.at(&dir()) && !last_back.is_some_and(|l| l.same(&Location::dir(p("temp1"))))
            },
        );
        s.add(
            "(go into a temp folder)",
            |a| {
                let _ = std::fs::create_dir(p("temp2"));
                a.open(Location::dir(p("temp2")), Nav::New, ViewState::default());
            },
            |a| a.at(&p("temp2")),
        );
        s.add(
            "(go back, so the temp folder waits ahead in the history)",
            |a| a.press(VK_LEFT, ALT),
            move |a| {
                let ahead = a.with_tab(|t| t.forward.last().cloned());
                a.at(&dir()) && ahead.is_some_and(|l| l.same(&Location::dir(p("temp2"))))
            },
        );
        s.add(
            "Forward to a deleted folder drops it from history instead of getting stuck",
            |a| {
                let _ = std::fs::remove_dir(p("temp2"));
                a.press(VK_RIGHT, ALT);
            },
            move |a| {
                let ahead = a.with_tab(|t| t.forward.last().cloned());
                a.at(&dir()) && !ahead.is_some_and(|l| l.same(&Location::dir(p("temp2"))))
            },
        );
        // The refresh has landed with exactly `names` selected.
        fn selected_exactly(a: &App, names: &[&str]) -> bool {
            let selected = a.list.selected_names();
            a.idle() && selected.len() == names.len() && names.iter().all(|n| selected.iter().any(|s| s == *n))
        }
        // Enter on the selected shortcuts, once the refresh that selected them has landed.
        let enter_when = |names: &'static [&'static str]| {
            move |a: &App| {
                if !selected_exactly(a, names) {
                    return false;
                }
                focus(a.list.hwnd());
                a.press(VK_RETURN, 0);
                true
            }
        };
        // The same for one of an item menu's own verbs, picked as the menu would pick it (no menu is shown).
        let verb_when = |verb: &'static str, names: &'static [&'static str]| {
            move |a: &App| {
                if !selected_exactly(a, names) {
                    return false;
                }
                a.item_verb(verb, &a.menu_items());
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
            move |a| a.at(&p("sub")) && clean(a.location()),
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
            one_tab_home,
        );
        s.add(
            "in a network folder that is not available, paste, new folder and a drag over its tab return at once",
            |a| {
                // As if this tab's folder had just failed to load (a real failure can take minutes to arrive).
                let offline = Location::dir("\\\\192.0.2.1\\share");
                a.with_tab(|t| t.location = offline.clone());
                a.show_unavailable(&offline, "The network path was not found.");
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
        let windows_before = Rc::new(RefCell::new(Vec::new()));
        let (before_act, before_done) = (windows_before.clone(), windows_before);
        s.add(
            "Alt+Enter in a tab that is not available opens nothing: there is no folder to show properties of",
            move |a| {
                *before_act.borrow_mut() = visible_windows();
                focus(a.list.hwnd());
                a.press(VK_RETURN, ALT);
            },
            move |_| {
                // Still nothing new after a while (the empty list's "" would have opened This PC's System page).
                if step_ms() < 1500 {
                    return false;
                }
                let before = before_done.borrow();
                visible_windows().iter().all(|h| before.contains(h))
            },
        );
        let reopen_home = move |a: &App| {
            a.with_tab(|t| t.location = Location::dir(dir()));
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
            "a bookmark whose drive letter is gone (a flash drive that came back under another letter) opens its folder \
             where the shortcut says it is, relative to itself (5.6)",
            |a| {
                // The folder under a drive letter of its own, as on a flash drive: the shortcut is made through the letter,
                // and then the letter goes, as it does when the drive comes back as another one.
                let used = unsafe { GetLogicalDrives() };
                let letter = (b'D'..=b'Z').rev().find(|l| used & (1 << (l - b'A')) == 0).unwrap_or(b'Z');
                let drive = format!("{}:", letter as char);
                let _ = std::fs::create_dir_all(p("pack\\target"));
                let (drive_w, pack_w) = (wide(&drive), wide(p("pack")));
                unsafe {
                    let _ = DefineDosDeviceW(DDD_NO_BROADCAST_SYSTEM, pcwstr(&drive_w), pcwstr(&pack_w));
                }
                make_shortcut(Path::new(&format!("{drive}\\go.lnk")), format!("{drive}\\target")); // both through the letter: Windows stores the way between them
                unsafe {
                    let gone = DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE | DDD_NO_BROADCAST_SYSTEM;
                    let _ = DefineDosDeviceW(gone, pcwstr(&drive_w), pcwstr(&pack_w));
                }
                a.open_shortcut(p("pack\\go.lnk"), false);
            },
            |a| a.at(&p("pack\\target")),
        );
        s.home("(back to the test folder)");
        s.add(
            "(remove them)",
            |_| {
                let _ = std::fs::remove_dir_all(p("pack"));
            },
            |_| !exists(&p("pack")),
        );
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
                drop(a.watch.take());
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
                let _ = std::fs::copy(system_dir().unwrap_or_default().join("cmd.exe"), p("tool.exe"));
                a.refresh(ViewState::default());
            },
            |a| {
                let sfi = file_info(&wide(p("tool.exe")), FILE_FLAGS_AND_ATTRIBUTES(0), SHGFI_SYSICONINDEX | SHGFI_SMALLICON).unwrap_or_default();
                let Some(row) = a.row_of("tool.exe") else { return false };
                let mut item = LVITEMW { mask: LVIF_IMAGE, iItem: row as i32, ..Default::default() };
                send(a.list.hwnd(), LVM_GETITEMW, 0, &mut item as *mut _ as isize);
                a.idle() && item.iImage == sfi.iIcon
            },
        );
        // Own icons are read for the rows on screen, a screenful at a time as the list scrolls, not for the whole
        // folder: System32 has 700 programs (4.7).
        s.add(
            "(a folder of 300 shortcuts, each with its own icon)",
            |a| {
                let _ = std::fs::create_dir(p("links"));
                for i in 0..300 {
                    make_shortcut(&p(&format!("links\\link {i}.lnk")), p("sub"));
                }
                a.open(Location::dir(p("links")), Nav::New, ViewState::default());
            },
            |a| a.at(&p("links")) && a.list.count() == 300,
        )
        .wait(20000);
        // Read for a few rows more than that many screenfuls at most.
        let icons_read = |a: &App, pages: usize| {
            let read = a.list.own_icon_count();
            let page = send(a.list.hwnd(), LVM_GETCOUNTPERPAGE, 0, 0) as usize;
            note(format!("{read} read, {page} rows to a screenful"));
            read > 0 && read <= pages * page + 4
        };
        s.add(
            "in a folder of 300 shortcuts, own icons are read only for the rows on screen (4.7)",
            |_| {},
            move |a| {
                let first = a.list.item(0).map(|it| it.name).unwrap_or_default();
                step_ms() >= 1000 && a.list.own_icon_known(&first) && icons_read(a, 1)
            },
        );
        s.add(
            "scrolled to the end, its last rows get their own icons too, and two screenfuls in all were read (4.7)",
            |a| {
                send(a.list.hwnd(), LVM_ENSUREVISIBLE, a.list.count() - 1, 0);
            },
            move |a| {
                let last = a.list.item(a.list.count() - 1).map(|it| it.name).unwrap_or_default();
                a.list.own_icon_known(&last) && icons_read(a, 2)
            },
        );
        s.add(
            "(remove them)",
            move |a| {
                a.open(Location::dir(dir()), Nav::New, ViewState::default());
                let _ = std::fs::remove_dir_all(p("links"));
            },
            move |a| a.at(&dir()) && !exists(&p("links")),
        )
        .wait(10000);
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
            is_directory(&p("Bookmarks")) && a.node_text(a.tree.root()) == "Bookmarks"
        });
        s.check("right-click menus can be built for This PC, a drive and a file", |a| {
            item_menu_at(a.wnd, &Location::ThisPc).is_some()
                && item_menu_at(a.wnd, &Location::dir("C:\\")).is_some()
                && item_menu_at(a.wnd, &Location::dir(p("file1.txt"))).is_some()
        });
        // An item menu's own verbs, picked as the menu would pick them (no menu is shown). A shortcut's Open file
        // location shows the folder its target is in, here, where Windows would open an Explorer window (4.4).
        s.add(
            "a shortcut's Open file location shows the target's folder here, with the target selected (not in Explorer)",
            move |a| {
                write_file(&p("sub\\target.txt"));
                make_shortcut(&p("far.lnk"), p("sub\\target.txt"));
                a.open(Location::dir(dir()), Nav::New, ViewState::selecting("far.lnk"));
            },
            verb_when("opencontaining", &["far.lnk"]),
        );
        let target_shown = |a: &App| a.at(&p("sub")) && a.list.selected_names() == [OsString::from("target.txt")];
        s.check("(the target's folder is shown, with the target selected)", target_shown);
        s.home("(back to the test folder)");
        s.add(
            "the same on a bookmark's menu in the tree",
            |a| {
                a.node_verb("opencontaining", &Location::dir(p("far.lnk")), true);
            },
            target_shown,
        );
        s.home("(back to the test folder)");
        s.add(
            "(remove them)",
            |a| {
                let _ = std::fs::remove_file(p("far.lnk"));
                let _ = std::fs::remove_file(p("sub\\target.txt"));
                a.refresh(ViewState::default());
            },
            |a| a.idle() && !a.listed("far.lnk"),
        );
        // This PC on the clipboard, put there again if another program took it: a shell item with no file path, as a
        // zip's or a phone's items are.
        fn pc_on_clipboard(a: &App) {
            const CF_HDROP: u32 = 15;
            unsafe {
                let items = RegisterClipboardFormatW(w!("Shell IDList Array"));
                if IsClipboardFormatAvailable(items).is_ok() && IsClipboardFormatAvailable(CF_HDROP).is_err() {
                    return;
                }
                if let Some(pc) = data_object_at(a.wnd, &Location::ThisPc) {
                    let _ = OleSetClipboard(&pc);
                }
            }
        }
        s.add(
            "Paste on empty space is offered for items without a file path, as copied from a zip or a phone",
            pc_on_clipboard,
            |a| {
                pc_on_clipboard(a);
                can_paste()
            },
        );
        s.add(
            "a tab made on a file, as qoqdir <file> makes one, shows the file's folder with the file selected",
            |a| a.new_tab(Location::dir(p("file1.txt")), true),
            move |a| {
                let here = Location::dir(dir());
                a.tabs() == 2
                    && a.at(&dir())
                    && a.list.selected_names() == [OsString::from("file1.txt")]
                    && text_of(a.address) == here.text().to_string_lossy()
                    && text_of(a.wnd) == here.display_name().to_string_lossy()
            },
        );
        s.add("(close it)", |a| a.close_tab(a.active()), one_tab_home);
        s.add(
            "a drag over a tab whose folder was never shown is refused at once (nobody has checked that folder)",
            |a| {
                a.new_tab(Location::dir(p("made")), false); // a second tab, right of the current one, as from --tab= or a middle-click
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
        // Ctrl says copy, so a drop that could move the file asks nothing: it copies at once (5.8). Without a key, the
        // same drop would ask with Windows' menu, which only a person can answer: that is checked by hand.
        s.add(
            "a drop with Ctrl held copies at once, without asking, though the file could be moved (5.8)",
            |a| {
                write_file(&p("held.txt"));
                let cursor = a.tab_point(a.active() + 1);
                let data = data_object_at(a.wnd, &Location::dir(p("held.txt")));
                let (Some(data), Some(target)) = (data, a.tab_drop.borrow().clone()) else { return };
                let (any, keys) = (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, MK_LBUTTON | MK_CONTROL);
                unsafe {
                    let mut effect = any;
                    let _ = target.DragEnter(&data, keys, cursor, &mut effect);
                    effect = any;
                    let _ = target.DragOver(keys, cursor, &mut effect);
                    effect = any;
                    let _ = target.Drop(&data, keys, cursor, &mut effect);
                }
            },
            |_| {
                if !exists(&p("made\\held.txt")) || !exists(&p("held.txt")) {
                    return false; // copied, not moved
                }
                let _ = std::fs::remove_file(p("made\\held.txt"));
                let _ = std::fs::remove_file(p("held.txt"));
                true
            },
        )
        .wait(10000);
        // A folder in the list takes a drop too (5.8): copy only here, so nothing asks.
        s.add(
            "dropping a file on a folder in the list puts it in that folder, whose row lights up while the file is \
             over it, and not afterwards (5.8)",
            |a| {
                write_file(&p("into.txt"));
                let cursor = a.row_point("sub");
                let data = data_object_at(a.wnd, &Location::dir(p("into.txt")));
                let (Some(data), Some(target)) = (data, a.list_drop.borrow().clone()) else { return };
                unsafe {
                    let mut effect = DROPEFFECT_COPY;
                    let _ = target.DragEnter(&data, MK_LBUTTON, cursor, &mut effect);
                    effect = DROPEFFECT_COPY;
                    let _ = target.DragOver(MK_LBUTTON, cursor, &mut effect);
                    note(if a.row_lit("sub") { "lit while over it" } else { "not lit while over it" }.into());
                    effect = DROPEFFECT_COPY;
                    let _ = target.Drop(&data, MK_LBUTTON, cursor, &mut effect);
                }
            },
            |a| {
                if !exists(&p("sub\\into.txt")) || noted() != "lit while over it" || a.row_lit("sub") {
                    return false;
                }
                let _ = std::fs::remove_file(p("sub\\into.txt"));
                let _ = std::fs::remove_file(p("into.txt"));
                true
            },
        )
        .wait(10000);
        // What a drag carries is read from its data, so it doesn't matter where it started, this list or another
        // window: the folder it carries refuses, and so do a file's row and the tab, which mean the folder it is in;
        // another folder's row takes it. No drop: a plain drop that could move asks, and only a person answers (5.8).
        s.add(
            "a drag that carries the folder sub, from this list or another window, takes another folder's row, but not \
             sub's own, nor a file's row or the tab of the folder sub is in (4.8)",
            |a| {
                let data = data_object_at(a.wnd, &Location::dir(p("sub")));
                let (list, tab) = (a.list_drop.borrow().clone(), a.tab_drop.borrow().clone());
                let (Some(data), Some(list), Some(tab)) = (data, list, tab) else { return };
                let any = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                let mut seen = Vec::new();
                unsafe {
                    let mut effect = any;
                    let _ = list.DragEnter(&data, MK_LBUTTON, a.row_point("sub"), &mut effect);
                    let refused = effect == DROPEFFECT_NONE && !a.row_lit("sub");
                    seen.push(if refused { "sub refused" } else { "sub taken" });
                    effect = any;
                    let _ = list.DragOver(MK_LBUTTON, a.row_point("made"), &mut effect);
                    let taken = effect != DROPEFFECT_NONE && a.row_lit("made");
                    seen.push(if taken { "made taken" } else { "made refused" });
                    effect = any;
                    let _ = list.DragOver(MK_LBUTTON, a.row_point("file1.txt"), &mut effect);
                    let refused = effect == DROPEFFECT_NONE && !a.row_lit("made");
                    seen.push(if refused { "file refused" } else { "file taken" });
                    let _ = list.DragLeave();
                    effect = any;
                    let _ = tab.DragEnter(&data, MK_LBUTTON, a.tab_point(a.active()), &mut effect);
                    seen.push(if effect == DROPEFFECT_NONE { "tab refused" } else { "tab taken" });
                    let _ = tab.DragLeave();
                }
                note(seen.join(", "));
            },
            |a| noted() == "sub refused, made taken, file refused, tab refused" && !a.row_lit("made"),
        );
        // While a drop would ask, the drag's tip says so, with no icon, not Windows' "Move to" with its arrow. With
        // Ctrl held it is the folder's own, which it writes again as the drag moves. No drop here: it would ask (5.8).
        s.add(
            "while a drop on a folder would ask, the drag's tip says Copy or move to it, with no icon; with Ctrl held \
             it is Windows' own, and once the drag has left there is none (5.8)",
            |a| {
                FLAG.set(false);
                let data = data_object_at(a.wnd, &Location::dir(p("file1.txt")));
                let (Some(data), Some(target)) = (data, a.list_drop.borrow().clone()) else { return };
                let any = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                unsafe {
                    let mut effect = any;
                    let _ = target.DragEnter(&data, MK_LBUTTON, a.row_point("sub"), &mut effect);
                    let asking = App::drag_tip(&data);
                    effect = any;
                    let _ = target.DragOver(MK_LBUTTON | MK_CONTROL, a.row_point("sub"), &mut effect);
                    let held = App::drag_tip(&data);
                    let _ = target.DragLeave();
                    let left = App::drag_tip(&data);
                    FLAG.set(asking == "Copy or move to %1 [sub]" && held == "(1) Copy to %1 [sub]" && left.is_empty());
                    note(format!("{asking} / {held} / {left}"));
                }
            },
            |_| FLAG.get(),
        );
        // Windows' hit test takes most of the column header for the first row, or for one scrolled behind it: a drag
        // over the header goes to the folder shown, and no row lights up (5.8). No drop: it would ask.
        s.add(
            "a drag over the list's column header goes to the folder shown, and no row lights up (5.8)",
            |a| {
                let header = HWND(send(a.list.hwnd(), LVM_GETHEADER, 0, 0) as _);
                let mut r = RECT::default();
                unsafe {
                    let _ = GetWindowRect(header, &mut r);
                }
                let at = POINTL { x: r.left + 20, y: (r.top + r.bottom) / 2 }; // over the Name column's title
                // From elsewhere: nothing here refuses it.
                let data = data_object_at(a.wnd, &Location::dir(p("made\\file1.txt")));
                let (Some(data), Some(target)) = (data, a.list_drop.borrow().clone()) else { return };
                let first = a.list.item(0).map(|it| it.name.to_string_lossy().into_owned()).unwrap_or_default();
                unsafe {
                    let mut effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                    let _ = target.DragEnter(&data, MK_LBUTTON, at, &mut effect);
                    let lit = if a.row_lit(&first) { ", the first row lit" } else { "" };
                    note(format!("{}{lit}", App::drag_tip(&data)));
                    let _ = target.DragLeave();
                }
            },
            |a| noted() == format!("Copy or move to %1 [{}]", a.list.location().display_name().to_string_lossy()),
        );
        // The shortcut still says folder, so only looking tells that its folder is gone. Its row is then like a file's.
        s.add(
            "(a shortcut in the list to a folder that is then deleted)",
            |_| {
                let _ = std::fs::create_dir(p("vanished"));
                make_shortcut(&p("vanished.lnk"), p("vanished"));
                let _ = std::fs::remove_dir(p("vanished"));
            },
            |a| a.listed("vanished.lnk") && !a.listed("vanished"),
        );
        s.add(
            "a shortcut in the list whose folder is gone doesn't light up, and a drop on it goes to the folder \
             shown (5.8)",
            |a| {
                let data = data_object_at(a.wnd, &Location::dir(p("made\\file1.txt")));
                let (Some(data), Some(target)) = (data, a.list_drop.borrow().clone()) else { return };
                unsafe {
                    let mut effect = DROPEFFECT_COPY;
                    let _ = target.DragEnter(&data, MK_LBUTTON, a.row_point("vanished.lnk"), &mut effect);
                    let taken = if effect == DROPEFFECT_NONE { "refused" } else { "taken" };
                    let lit = if a.row_lit("vanished.lnk") { "lit" } else { "not lit" };
                    note(format!("{taken}, {lit}"));
                    let _ = target.DragLeave();
                }
                let _ = std::fs::remove_file(p("vanished.lnk"));
            },
            |a| noted() == "taken, not lit" && !a.listed("vanished.lnk"),
        );
        s.add("(close the extra tab)", |a| a.close_tab(a.active() + 1), |a| a.idle() && a.tabs() == 1);
        s.add(
            "middle-click on a folder opens it in a new background tab",
            |a| {
                if let Some(row) = a.row_of("sub") {
                    let mut r = RECT { left: LVIR_LABEL as i32, ..Default::default() };
                    send(a.list.hwnd(), LVM_GETITEMRECT, row, &mut r as *mut _ as isize);
                    send(a.list.hwnd(), WM_MBUTTONUP, 0, packed((r.left + r.right) / 2, (r.top + r.bottom) / 2));
                }
            },
            move |a| a.tabs() == 2 && a.tab_path(a.active() + 1).same(&Location::dir(p("sub"))) && a.at(&dir()),
        );
        s.add("(close that tab)", |a| a.close_tab(a.active() + 1), |a| a.idle() && a.tabs() == 1);
        s.add(
            "middle-click in the tree opens a new background tab",
            |a| {
                let desktop = a.tree.next_sibling(a.tree.root()); // Desktop (after Bookmarks)
                let r = a.node_rect(desktop, true);
                send(a.tree.hwnd(), WM_MBUTTONUP, 0, packed((r.left + r.right) / 2, (r.top + r.bottom) / 2));
            },
            move |a| {
                let desktop = known_folder(&FOLDERID_Desktop).map(Location::dir).unwrap_or(Location::ThisPc);
                a.tabs() == 2 && a.tab_path(a.active() + 1).same(&desktop) && a.at(&dir())
            },
        );
        s.add("(close that tab too)", |a| a.close_tab(a.active() + 1), |a| a.idle() && a.tabs() == 1);
        s.check("the tree lists the drives under This PC", |a| a.tree.child(a.tree.last_root()) != 0);
        s.check("root tree nodes have an expand arrow", |a| {
            // The arrow sits in the indent left of the icon; TVS_HASBUTTONS gives roots one only with TVS_LINESATROOT.
            let tree = a.tree.hwnd();
            let r = a.node_rect(a.tree.root(), false); // the whole Bookmarks row
            let indent = send(tree, TVM_GETINDENT, 0, 0) as i32;
            let mut hit = TVHITTESTINFO { pt: POINT { x: r.left + indent / 2, y: (r.top + r.bottom) / 2 }, ..Default::default() };
            send(tree, TVM_HITTEST, 0, &mut hit as *mut _ as isize);
            hit.flags.0 & TVHT_ONITEMBUTTON.0 != 0
        });
        s.add(
            "expanding a tree folder lists its subfolders",
            |a| {
                let desktop = a.tree.next_sibling(a.tree.root()); // after Bookmarks
                send(a.tree.hwnd(), TVM_EXPAND, TVE_EXPAND.0 as usize, desktop);
            },
            |a| {
                let desktop = known_folder(&FOLDERID_Desktop).map(Location::dir).unwrap_or(Location::ThisPc);
                let expected = list_folder(&desktop, false, None).items.iter().filter(|i| i.is_dir()).count();
                let root = a.tree.next_sibling(a.tree.root()); // Desktop
                let shown = a.tree.siblings(a.tree.child(root)).count();
                // Done when the subfolders are shown, or the node knows it has none.
                if expected > 0 { shown == expected } else { a.tree_item(root, TVIF_CHILDREN).cChildren.0 == 0 }
            },
        );
        s.add(
            "the user folders arrive from a worker, between Bookmarks and This PC",
            |a| {
                a.tree.fill(); // as at startup and on a drive change
                // Nothing asks Windows about the user folders here (4.1): they arrive by message, later.
                FLAG.set(a.node_texts(a.tree.root()) == "Bookmarks,This PC");
            },
            |a| {
                let sfi = known_folder(&FOLDERID_Desktop)
                    .and_then(|desktop| file_info(&wide(desktop), FILE_FLAGS_AND_ATTRIBUTES(0), SHGFI_DISPLAYNAME))
                    .unwrap_or_default();
                let roots = a.node_texts(a.tree.root());
                let desktop = from_wide(&sfi.szDisplayName).to_string_lossy().into_owned();
                FLAG.get() && roots.starts_with(&format!("Bookmarks,{desktop},")) && roots.ends_with(",This PC")
            },
        );
        s.add(
            "a drive change during the first read of the drives still lists them under This PC",
            |a| {
                a.tree.fill(); // This PC's read starts
                volume_change(a, DBT_DEVICEARRIVAL); // before it lands
            },
            |a| {
                let last = a.tree.last_root(); // This PC
                let expanded = a.expanded(last);
                a.tree.child(last) != 0 && expanded
            },
        );
        s.add(
            "an empty Bookmarks node keeps its arrow, and F5 lists a bookmark added later",
            |a| {
                FLAG.set(a.tree_item(a.tree.root(), TVIF_CHILDREN).cChildren.0 == 1); // the startup read found Bookmarks empty
                let _ = std::fs::create_dir(p("Bookmarks\\one"));
                a.press(VK_F5, 0);
            },
            |a| FLAG.get() && a.bookmark_nodes() == "one",
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
                let own = file_info(&wide(p("Bookmarks\\sub.lnk")), FILE_FLAGS_AND_ATTRIBUTES(0), SHGFI_SYSICONINDEX | SHGFI_SMALLICON).unwrap_or_default();
                let sub = a.tree.next_sibling(a.tree_one());
                a.tree_item(sub, TVIF_IMAGE).iImage == own.iIcon
            },
        );
        s.add(
            "choosing a bookmark shortcut in the tree opens its folder",
            |a| {
                a.tree_select(a.tree_one());
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
            "a folder made in Bookmarks shows up in the tree by itself (5.5)",
            |a| {
                a.cancel_rename(); // the box from the step before
                let _ = std::fs::create_dir(p("Bookmarks\\two"));
            },
            |a| a.bookmark_nodes() == "one,two,sub",
        );
        s.add(
            "collapsing a node while a child is being read drops the late result",
            |a| {
                let _ = std::fs::create_dir(p("Bookmarks\\one\\deep"));
                send(a.tree.hwnd(), TVM_EXPAND, TVE_EXPAND.0 as usize, a.tree_one()); // its read starts
                a.tree_select(a.tree.root());
                a.tree_key(VK_LEFT); // collapse Bookmarks: deletes "one" before its read lands
                a.tree_key(VK_RIGHT); // and read it fresh
            },
            |a| {
                // Both reads landed: no late "deep" under Bookmarks, and every node the tree counts is reachable.
                if step_ms() < 400 || a.bookmark_nodes() != "one,two,sub" {
                    return false;
                }
                if a.tree.siblings(a.tree_one()).any(|n| a.tree.child(n) != 0) {
                    return false;
                }
                a.count_nodes(a.tree.root()) == send(a.tree.hwnd(), TVM_GETCOUNT, 0, 0) as usize
            },
        );
        s.add(
            "(expand a bookmark folder in the tree)",
            |a| {
                send(a.tree.hwnd(), TVM_EXPAND, TVE_EXPAND.0 as usize, a.tree_one());
            },
            |a| a.tree.child(a.tree_one()) != 0, // "deep" is shown
        );
        s.add(
            "collapsing a folder while F5 reads it again leaves it collapsed, with its arrow (4.5)",
            |a| {
                a.tree_select(a.tree_one()); // open
                a.press(VK_F5, 0); // reads every open node again, "one" among them
                a.tree_key(VK_LEFT); // collapsed before that read lands
            },
            move |a| {
                let one = a.tree_one();
                if step_ms() < 400 || !a.at(&dir()) || one == 0 {
                    return false;
                }
                let expanded = a.expanded(one);
                !expanded && a.tree.child(one) == 0 && a.tree_item(one, TVIF_CHILDREN).cChildren.0 == 1 && a.bookmark_nodes() == "one,two,sub"
            },
        );
        s.add(
            "(expand it again)",
            |a| {
                // As the user would: TVM_EXPAND tells nothing about a node expanded before.
                a.tree_select(a.tree_one());
                a.tree_key(VK_RIGHT);
            },
            |a| a.tree.child(a.tree_one()) != 0,
        );
        s.add(
            "a drive coming or going re-reads This PC and leaves the rest of the tree as it was (4.5)",
            |a| volume_change(a, DBT_DEVICEARRIVAL),
            |a| {
                let one = a.tree_one();
                let expanded = a.expanded(one);
                a.bookmark_nodes() == "one,two,sub" && one != 0 && a.tree.child(one) != 0 && expanded && a.tree.child(a.tree.last_root()) != 0
            },
        );
        // Made where no watch sees them (Bookmarks' watch takes only its own children, and the list shows another
        // folder): only the read when the node opens again can show them, and never the hidden one (5.5).
        s.add(
            "collapsing and expanding a tree folder shows its new subfolders, and not its hidden ones",
            |a| {
                let _ = std::fs::create_dir(p("Bookmarks\\one\\fresh"));
                let _ = std::fs::create_dir(p("Bookmarks\\one\\hidden"));
                let hidden = wide(p("Bookmarks\\one\\hidden"));
                unsafe {
                    let attrs = GetFileAttributesW(pcwstr(&hidden));
                    let with_hidden = FILE_FLAGS_AND_ATTRIBUTES(attrs | FILE_ATTRIBUTE_HIDDEN.0);
                    let _ = SetFileAttributesW(pcwstr(&hidden), with_hidden);
                }
                a.tree_select(a.tree_one());
                a.tree_key(VK_LEFT); // collapse, as the user would
                a.tree_key(VK_RIGHT); // and expand again
            },
            |a| a.node_texts(a.tree.child(a.tree_one())) == "deep,fresh",
        );
        s.add(
            "(remove them, and read the folder again)",
            |a| {
                let _ = std::fs::remove_dir(p("Bookmarks\\one\\fresh"));
                let _ = std::fs::remove_dir(p("Bookmarks\\one\\hidden"));
                a.tree_select(a.tree_one());
                a.tree_key(VK_LEFT);
                a.tree_key(VK_RIGHT);
            },
            |a| a.node_texts(a.tree.child(a.tree_one())) == "deep",
        );
        // A change that comes while a node is being read: that read may have listed the folder before it, so one more
        // follows (5.5). Made where no watch sees it, only that second read can show it.
        s.add(
            "a change to a tree folder while it is being read shows once that read is in (5.5)",
            |a| {
                let one = Location::dir(p("Bookmarks\\one"));
                a.tree.refresh_folder(&one); // a read of "one" goes out...
                let msg = a.tree.batch_message();
                let start = Instant::now(); // ...and lists the folder: its result waits in the queue
                let mut m = MSG::default();
                while !unsafe { PeekMessageW(&mut m, Some(a.wnd), msg, msg, PM_NOREMOVE) }.as_bool()
                    && start.elapsed() < Duration::from_secs(2)
                {
                    std::thread::sleep(Duration::from_millis(5));
                }
                let _ = std::fs::create_dir(p("Bookmarks\\one\\fresh"));
                a.tree.refresh_folder(&one); // the change, as a watch tells it, while that read is out
            },
            |a| a.node_texts(a.tree.child(a.tree_one())) == "deep,fresh",
        );
        s.add(
            "(remove it)",
            |a| {
                let _ = std::fs::remove_dir(p("Bookmarks\\one\\fresh"));
                a.tree.refresh_folder(&Location::dir(p("Bookmarks\\one")));
            },
            |a| a.node_texts(a.tree.child(a.tree_one())) == "deep",
        );
        s.add(
            "F5 reads the folders open in the tree again: one that is the same stays as it is (\"one\" stays open), one \
             that changed shows its new subfolder (5.5)",
            |a| {
                let _ = std::fs::create_dir(p("Bookmarks\\one\\new"));
                a.press(VK_F5, 0);
            },
            |a| {
                let one = a.tree_one(); // open, with "deep"
                let expanded = a.expanded(one);
                a.bookmark_nodes() == "one,two,sub" && a.node_texts(a.tree.child(one)) == "deep,new" && expanded
            },
        );
        s.check("(the status bar says Refreshed)", |a| a.status_message() == "Refreshed");
        let new_node = |a: &App| a.tree.next_sibling(a.tree.child(a.tree_one()));
        s.add(
            "(open the folder new in the tree: it has no subfolders, so its arrow goes)",
            move |a| {
                send(a.tree.hwnd(), TVM_EXPAND, TVE_EXPAND.0 as usize, new_node(a));
            },
            move |a| a.tree_item(new_node(a), TVIF_CHILDREN).cChildren.0 == 0,
        );
        s.add(
            "the Refresh button does the same, and reads again a folder known to have no subfolders: it shows the one \
             made since",
            |a| {
                let _ = std::fs::create_dir(p("Bookmarks\\one\\new\\inside"));
                send(a.refresh, BM_CLICK, 0, 0);
            },
            move |a| a.node_texts(a.tree.child(new_node(a))) == "inside",
        );
        s.add(
            "(open Bookmarks\\one in the list)",
            |a| a.open(Location::dir(p("Bookmarks\\one")), Nav::New, ViewState::default()),
            |a| a.at(&p("Bookmarks\\one")),
        );
        s.add(
            "a subfolder made in the folder shown shows up in the tree by itself: its open node is read again (5.5)",
            |_| {
                let _ = std::fs::create_dir(p("Bookmarks\\one\\later"));
            },
            |a| a.node_texts(a.tree.child(a.tree_one())) == "deep,later,new",
        );
        s.add(
            "a shortcut made in the folder shown shows up in the tree by itself too, after the subfolders, with its own icon \
             and no arrow (5.6)",
            |_| make_shortcut(&p("Bookmarks\\one\\sub.lnk"), p("sub")),
            |a| {
                let one = a.tree_one();
                if a.node_texts(a.tree.child(one)) != "deep,later,new,sub" {
                    return false;
                }
                let own = file_info(&wide(p("Bookmarks\\one\\sub.lnk")), FILE_FLAGS_AND_ATTRIBUTES(0), SHGFI_SYSICONINDEX | SHGFI_SMALLICON).unwrap_or_default();
                let mut sub = a.tree.child(one); // the last child
                while a.tree.next_sibling(sub) != 0 {
                    sub = a.tree.next_sibling(sub);
                }
                let item = a.tree_item(sub, TVIF_IMAGE | TVIF_CHILDREN);
                item.iImage == own.iIcon && item.cChildren.0 == 0
            },
        );
        s.home("(back to the test folder)");
        s.add(
            "a folder deleted while the list shows another leaves the tree once its parent is read again, as it is when a \
             node's own menu closes",
            |a| {
                let _ = std::fs::remove_dir(p("Bookmarks\\one\\later"));
                a.tree.refresh_folder(&Location::dir(p("Bookmarks\\one")));
            },
            |a| a.node_texts(a.tree.child(a.tree_one())) == "deep,new,sub",
        );
        s.add(
            "This PC in the tree takes no drop",
            |a| {
                let data = data_object_at(a.wnd, &Location::dir(p("file1.txt")));
                let mut effect = DROPEFFECT_COPY;
                if let (Some(data), Some(target)) = (data, a.tree_drop.borrow().clone()) {
                    unsafe {
                        let _ = target.DragEnter(&data, MK_LBUTTON, a.node_point(a.tree.last_root()), &mut effect);
                        let _ = target.DragLeave();
                    }
                }
                EFFECT.set(effect.0);
            },
            |a| {
                EFFECT.get() == DROPEFFECT_NONE.0
                    && a.tree.drop_highlight() == 0
                    && a.status_message() == "Only a folder on this PC takes a drop"
            },
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
            |a| EFFECT.get() == DROPEFFECT_COPY.0 && a.tree.drop_highlight() == a.tree_two(),
        );
        s.check("held over the closed folder, the drag opens it after a moment", |a| {
            let mut effect = DROPEFFECT_COPY; // Windows asks again many times a second while the drag is held there
            if let Some(target) = a.tree_drop.borrow().clone() {
                unsafe {
                    let _ = target.DragOver(MK_LBUTTON, a.node_point(a.tree_two()), &mut effect);
                }
            }
            a.tree.child(a.tree_two()) != 0 // "inner" is shown
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
            |a| exists(&p("Bookmarks\\two\\file1.txt")) && a.tree.drop_highlight() == 0,
        )
        .wait(10000);
        s.add(
            "a bookmark shortcut to a folder on this PC takes a drop into that folder",
            |a| {
                let shortcut = a.node_point(a.tree.next_sibling(a.tree_two())); // "sub", to the folder sub
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
        // Nor does a drag go back where its files are from the tree, which says why (4.8, 5.5).
        s.add(
            "in the tree, a drag is refused on the folder its file is in and on a bookmark to the folder it carries, \
             and the status bar says why (4.8)",
            |a| {
                let file = data_object_at(a.wnd, &Location::dir(p("Bookmarks\\two\\file1.txt")));
                let folder = data_object_at(a.wnd, &Location::dir(p("sub")));
                let (Some(file), Some(folder), Some(target)) = (file, folder, a.tree_drop.borrow().clone()) else {
                    return;
                };
                let shortcut = a.node_point(a.tree.next_sibling(a.tree_two())); // "sub", to the folder sub
                unsafe {
                    let mut effect = DROPEFFECT_COPY;
                    let _ = target.DragEnter(&file, MK_LBUTTON, a.node_point(a.tree_two()), &mut effect);
                    let two = if effect == DROPEFFECT_NONE { "two refused" } else { "two taken" };
                    let mut seen = format!("{two}: {}", a.status_message());
                    let _ = target.DragLeave();
                    effect = DROPEFFECT_COPY;
                    let _ = target.DragEnter(&folder, MK_LBUTTON, shortcut, &mut effect);
                    let sub = if effect == DROPEFFECT_NONE { "sub refused" } else { "sub taken" };
                    seen += &format!(" / {sub}: {}", a.status_message());
                    let _ = target.DragLeave();
                    note(seen);
                }
            },
            |_| noted() == "two refused: The files come from there / sub refused: A folder can't go into itself",
        );
        s.add(
            "expanding a folder with 10,000 subfolders in the tree keeps the window responsive", // WinSxS has 20,000
            |a| {
                for i in 0..10000 {
                    let _ = std::fs::create_dir(p(&format!("Bookmarks\\two\\inner\\{i}")));
                }
                send(a.tree.hwnd(), TVM_EXPAND, TVE_EXPAND.0 as usize, a.tree.child(a.tree_two())); // "inner"
                T0.set(Some(Instant::now())); // the last timer tick
                KEY_MS.set(0); // the longest the window went without one
            },
            |a| {
                KEY_MS.set(KEY_MS.get().max(T0.get().map_or(0, |t| t.elapsed().as_millis())));
                T0.set(Some(Instant::now()));
                let shown = a.tree.siblings(a.tree.child(a.tree.child(a.tree_two()))).count();
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
                saved.items.len() == a.tabs() + 1
                    && saved.items.last().is_some_and(|it| it.name == "keep.txt")
                    && (0..a.tabs()).all(|i| {
                        shortcut_target(&p("Bookmarks\\Tabs").join(&saved.items[i].name))
                            .is_some_and(|(target, folder)| folder && target.same(&a.tab_path(i)))
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
            one_tab_home,
        );
        // Two saves at once, as Ctrl+S on a worker and then closing with "Save these tabs?" answered Yes: the later
        // one waits for the first and then replaces what it wrote, so none of the first's shortcuts are left (5.7).
        let saves = Arc::new(AtomicUsize::new(0)); // saves finished
        let overlap = Rc::new(Cell::new(None::<bool>)); // whether the second started while the first was writing
        let saves_act = saves.clone();
        s.add(
            "a save of the tabs that starts while another is still writing waits for it, and replaces all it wrote",
            move |_| {
                // Long enough to still be writing when the next starts.
                let (dir, many, saves) = (p("tabsrace"), vec![Location::dir(p("sub")); 300], saves_act.clone());
                in_background(move || {
                    save_folder_shortcuts(&dir, &many);
                    saves.fetch_add(1, Ordering::SeqCst);
                });
            },
            move |_| {
                let Some(overlapped) = overlap.get() else {
                    // The second save starts once the first is writing.
                    if !exists(&p("tabsrace\\1 sub.lnk")) {
                        return false;
                    }
                    overlap.set(Some(saves.load(Ordering::SeqCst) == 0));
                    let (dir, made, saves) = (p("tabsrace"), Location::dir(p("made")), saves.clone());
                    in_background(move || {
                        save_folder_shortcuts(&dir, &[made]);
                        saves.fetch_add(1, Ordering::SeqCst);
                    });
                    return false;
                };
                if saves.load(Ordering::SeqCst) < 2 {
                    return false;
                }
                let left: Vec<OsString> = std::fs::read_dir(p("tabsrace"))
                    .map(|d| d.filter_map(|e| e.ok().map(|e| e.file_name())).collect())
                    .unwrap_or_default();
                note(if overlapped {
                    format!("{} shortcut(s) left", left.len())
                } else {
                    "the first save ended before the second started".into()
                });
                overlapped && left == [OsString::from("1 made.lnk")]
            },
        )
        .wait(30000);
        s.add("(remove them)", |_| { let _ = std::fs::remove_dir_all(p("tabsrace")); }, |_| !exists(&p("tabsrace")));
        s.add(
            "Bookmark this folder on a tab's menu writes a shortcut to the tab's folder in Bookmarks and shows it in the \
             tree (5.6)",
            |a| a.bookmark_tab(a.active()),
            move |a| {
                let name = dir().file_name().unwrap_or_default().to_string_lossy().into_owned();
                exists(&p(&format!("Bookmarks\\{name}.lnk"))) && a.bookmark_nodes().contains(&name)
            },
        );
        s.add(
            "(remove it)",
            move |a| {
                let name = dir().file_name().unwrap_or_default().to_string_lossy().into_owned();
                let _ = std::fs::remove_file(p(&format!("Bookmarks\\{name}.lnk")));
                a.tree.refresh_folder(a.tree.bookmarks());
            },
            |a| a.bookmark_nodes() == "one,two,sub",
        );
        // These two wait for Explorer to pass on another program's change, which has taken it more than 5 s here
        // after this test's own floods of changes (10,000 folders, 300 shortcuts): not a false alarm when the
        // machine is busy.
        s.add(
            "a shortcut another program makes in Bookmarks (or an Alt+drag onto its node) shows in the tree while another \
             folder is shown (5.5)",
            |_| make_shortcut(&p("Bookmarks\\dropped.lnk"), p("sub")),
            |a| a.bookmark_nodes() == "one,two,dropped,sub",
        )
        .wait(20000);
        s.add(
            "(and leaves the tree when deleted)",
            |_| {
                let _ = std::fs::remove_file(p("Bookmarks\\dropped.lnk"));
            },
            |a| a.bookmark_nodes() == "one,two,sub",
        )
        .wait(20000);

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
                let ms = T0.get().map_or(0, |t| t.elapsed().as_millis());
                note(format!("load {ms} ms"));
                ms < 2000 // 4.7: under 1 s, with a margin for a busy PC
            },
        )
        .wait(30000);
        s.add(
            "filters 10,000 files, showing the matches from the top even when scrolled to the bottom",
            |a| {
                a.list.ensure_visible(Some(9999));
                let t = Instant::now();
                set_text(a.filter, "99");
                KEY_MS.set(t.elapsed().as_millis());
                note(format!("filter {} ms", KEY_MS.get()));
            },
            |a| {
                a.list.count() == (0..10000).filter(|i: &i32| i.to_string().contains("99")).count()
                    && a.list.top_row() == 0
                    && KEY_MS.get() < 200 // 4.7: under 0.1 s, with a margin
            },
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
                let mut slowest = 0u128;
                for (key, name) in [(SortKey::Name, "name"), (SortKey::Type, "type"), (SortKey::Modified, "date"), (SortKey::Size, "size")] {
                    let t = Instant::now();
                    a.list.set_sort(key, false); // descending: the list is in name order already
                    let ms = t.elapsed().as_millis();
                    slowest = slowest.max(ms);
                    text += &format!(" {name} {ms}");
                }
                KEY_MS.set(slowest);
                note(text + " ms");
                a.list.set_sort(SortKey::Name, true);
            },
            |a| a.list.count() == 10000 && KEY_MS.get() < 200, // 4.7: each under 0.1 s, with a margin
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
                // One read a press; a change another program makes in the folder meanwhile can add a refresh or two.
                a.idle() && a.active() == 0 && (30..=32).contains(&done) && wasted <= 3
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
                std::thread::sleep(Duration::from_millis(600)); // this thread waits: the read ends, and Windows reports the file to no one
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
        // Shown by a tab switch, This PC must get its own watch and the folder shown before must lose its, with a
        // refresh only that watch asked for: a change there would reload This PC for nothing, and This PC's own changes
        // would go unseen (4.5).
        s.add(
            "a refresh only the old folder's watch asked for goes with that watch: This PC loads once (4.5)",
            |a| {
                COUNT.set(a.loads_done.get());
                a.on_folder_changed(); // stands in for a change reported just before the switch
                a.new_tab(Location::ThisPc, true);
            },
            |a| {
                note(format!("{} load(s)", a.loads_done.get() - COUNT.get()));
                a.at_pc() && !a.refresh_queued.get() && a.loads_done.get() == COUNT.get() + 1
                    && a.tabs() == 2 && a.active() == 1
            },
        );
        s.add(
            "switching to a This PC tab drops the other tab's watch: a change in that folder reloads nothing (4.5)",
            |a| {
                COUNT.set(a.loads_done.get());
                write_file(&p("big\\zz unwatched.dat"));
            },
            |a| {
                note(format!("{} reload(s)", a.loads_done.get() - COUNT.get()));
                // 0.8 s of nothing (the watch's refresh comes at 400 ms).
                step_ms() >= 800 && a.loads_done.get() == COUNT.get()
            },
        );
        s.add(
            "(remove that file and close the tab)",
            |a| {
                let _ = std::fs::remove_file(p("big\\zz unwatched.dat"));
                a.close_tab(1);
            },
            |a| a.at(&p("big")) && a.tabs() == 1 && !a.listed("zz unwatched.dat"),
        );
        // Any other refresh stays: This PC lists the drives.
        s.add(
            "a drive that comes while This PC is being read gets This PC read again (4.5)",
            |a| {
                COUNT.set(a.loads_done.get());
                a.on_folder_changed(); // a change the old watch reported first: the drive's refresh is still kept
                a.new_tab(Location::ThisPc, true);
                volume_change(a, DBT_DEVICEARRIVAL); // before it lands
            },
            |a| {
                note(format!("{} load(s)", a.loads_done.get() - COUNT.get()));
                a.at_pc() && !a.refresh_queued.get() && a.loads_done.get() == COUNT.get() + 2
                    && a.tabs() == 2 && a.active() == 1
            },
        );
        s.add("(close that tab)", |a| a.close_tab(1), |a| a.at(&p("big")) && a.tabs() == 1);
        // A switch empties the list, so the list can't say which folder the watch is on: the watch says it.
        s.add(
            "a tab switch that another move overtakes still moves the watch to the folder shown (4.5)",
            |a| {
                a.new_tab(Location::dir(p("big")), true); // the move below overtakes its read
                a.open(Location::ThisPc, Nav::New, ViewState::default());
            },
            |a| {
                a.at_pc() && a.tabs() == 2 && a.active() == 1
                    && a.watch.borrow().as_ref().is_some_and(|w| w.watches(&Location::ThisPc))
            },
        );
        s.add("(close that tab too)", |a| a.close_tab(1), |a| a.at(&p("big")) && a.tabs() == 1);
        // The old watch also watched the folder above its own, for that folder going: what it saw there still counts.
        s.add(
            "going up keeps a refresh the old watch asked for: it watched the folder above too (4.5)",
            move |a| {
                COUNT.set(a.loads_done.get());
                a.on_folder_changed(); // stands in for a folder next to big going, which big's watch reports
                a.open(Location::dir(dir()), Nav::New, ViewState::default());
            },
            move |a| {
                note(format!("{} load(s)", a.loads_done.get() - COUNT.get()));
                a.at(&dir()) && !a.refresh_queued.get() && a.loads_done.get() == COUNT.get() + 2
            },
        );
        s.add(
            "(back into big)",
            |a| a.open(Location::dir(p("big")), Nav::New, ViewState::default()),
            |a| a.at(&p("big")),
        );
        // The switch emptied the list at once: a move that takes over and fails must not leave it empty.
        s.add(
            "a tab switch overtaken by a move that fails still shows the tab's folder, and why the move failed (4.2)",
            |a| {
                a.new_tab(Location::dir(p("sub")), true); // the move below overtakes its read
                a.open(Location::dir(p("no such folder")), Nav::New, ViewState::default());
            },
            |a| {
                let sub = Location::dir(p("sub"));
                a.idle() && a.tabs() == 2 && a.active() == 1 && a.list.location().same(&sub)
                    && a.watch.borrow().as_ref().is_some_and(|w| w.watches(&sub))
                    && a.status_message().contains("no such folder")
            },
        );
        s.add("(close that tab)", |a| a.close_tab(1), |a| a.at(&p("big")) && a.tabs() == 1);
        // The tab comes back as the switch would have shown it: the scroll exactly, not moved to the focused file.
        s.add("(a second tab on big)", |a| a.new_tab(Location::dir(p("big")), true), |a| {
            a.at(&p("big")) && a.tabs() == 2 && a.active() == 1
        });
        s.add(
            "(it focuses a file, is scrolled away from it, and is left for the first tab)",
            |a| {
                focus(a.list.hwnd());
                a.select_one("item 5000.dat");
                a.list.scroll_to_row(100); // as with the mouse wheel
                TOP.set(a.list.top_row());
                a.switch_tab(0);
            },
            |a| a.idle() && a.active() == 0,
        );
        s.add(
            "a tab whose switch a failed move overtakes comes back with its selection and scroll position (4.6)",
            |a| {
                a.switch_tab(1);
                a.open(Location::dir(p("no such folder")), Nav::New, ViewState::default());
            },
            |a| {
                a.idle()
                    && a.active() == 1
                    && a.list.shows(&Location::dir(p("big")))
                    && a.focused_is("item 5000.dat")
                    && a.list.top_row() == TOP.get()
            },
        );
        s.add("(close that tab)", |a| a.close_tab(1), |a| a.at(&p("big")) && a.tabs() == 1);
        // An emptied list shows no folder, not This PC: a This PC tab left meanwhile keeps what it remembered.
        let drive = Rc::new(RefCell::new(String::new()));
        s.add("(a This PC tab)", |a| a.new_tab(Location::ThisPc, true), |a| {
            a.at_pc() && a.active() == 1 && a.list.count() > 0
        });
        s.add(
            "(its last drive selected, remembered as the tab is left and shown again)",
            {
                let drive = drive.clone();
                move |a| {
                    focus(a.list.hwnd());
                    // Not the first row, which takes the focus by itself.
                    let name = a.list.item(a.list.count() - 1).map(|it| it.name).unwrap_or_default();
                    *drive.borrow_mut() = name.to_string_lossy().into_owned();
                    a.select_one(&drive.borrow());
                    a.switch_tab(0);
                    a.switch_tab(1);
                }
            },
            {
                let drive = drive.clone();
                move |a| a.at_pc() && a.active() == 1 && a.focused_is(&drive.borrow())
            },
        );
        s.add(
            "a This PC tab left while its list is emptied keeps its selection (4.6)",
            |a| {
                a.list.clear(); // as while a tab switch's read is out
                a.switch_tab(0);
                a.switch_tab(1);
            },
            move |a| a.at_pc() && a.active() == 1 && a.focused_is(&drive.borrow()),
        );
        s.add("(close that tab too)", |a| a.close_tab(1), |a| a.at(&p("big")) && a.tabs() == 1);
        // On a share that is off, a read of the tab's folder waits 20-40 s. A move that overtakes a tab switch lets the
        // switch's read go on, and the tab shows what it finds as soon as it is back: the folder is not read a second
        // time (4.2). A read still out is stood in for by its number, so the test needs no network.
        const STUCK_READ: u32 = u32::MAX; // no real read gets this number
        // To the second tab, and at once on to another folder (most often one that is not there), while the switch's
        // read is still out.
        fn overtake(a: &App, to: &str) {
            a.switch_tab(1);
            a.open(Location::dir(p(to)), Nav::New, ViewState::default());
            if let Some(kept) = a.st.borrow_mut().kept.as_mut() {
                kept.generation = STUCK_READ; // the real read comes back as one replaced, and is dropped
            }
        }
        // The stuck read comes back: sub, or the error it ran into; `changed`: sub changed while it was read.
        fn read_back(a: &App, error: Option<windows::core::Error>, changed: bool) {
            let found = Listing {
                location: Location::dir(p("sub")),
                written: if changed { 0 } else { folder_written(&p("sub")) },
                error,
                ..Default::default()
            };
            a.on_loaded(found, STUCK_READ);
        }
        // The move failed: only the switch's read is out.
        fn move_done(a: &App) -> bool {
            a.st.borrow().pending.is_none()
        }
        // The first tab, which the steps switch away from and back to, shows the test folder: quick to read each time.
        s.home("(the first tab on the test folder)");
        s.add("(a second tab on sub)", |a| a.new_tab(Location::dir(p("sub")), true), |a| {
            a.at(&p("sub")) && a.tabs() == 2 && a.active() == 1
        });
        s.first_tab();
        s.add(
            "a failed move after a tab switch reads the tab's folder no second time: the switch's read is shown (4.2)",
            |a| {
                COUNT.set(a.generation.get());
                a.switch_tab(1);
                a.open(Location::dir(p("missing zero")), Nav::New, ViewState::default());
            },
            |a| {
                note(format!("{} read(s)", a.generation.get() - COUNT.get()));
                a.idle()
                    && a.active() == 1
                    && a.list.shows(&Location::dir(p("sub")))
                    && a.status_message().contains("missing zero")
                    && a.generation.get() == COUNT.get() + 2
            },
        );
        s.first_tab();
        s.add(
            "a switch's read that is back while the move is still out shows the tab's folder at once (4.2)",
            |a| {
                COUNT.set(a.generation.get());
                overtake(a, "missing one");
                read_back(a, None, false);
                let at_once = a.list.shows(&Location::dir(p("sub"))) && a.is_loading();
                note(if at_once { "shown at once" } else { "held back" }.into());
            },
            |a| {
                noted() == "shown at once"
                    && a.idle()
                    && a.active() == 1
                    && a.list.shows(&Location::dir(p("sub")))
                    && a.status_message().contains("missing one")
                    && a.generation.get() == COUNT.get() + 2
            },
        );
        s.first_tab();
        s.add(
            "a failed move while the switch's read is still out leaves the tab waiting for it, reading no more (4.2)",
            |a| {
                COUNT.set(a.generation.get());
                overtake(a, "missing two");
            },
            |a| {
                note(format!("{} read(s)", a.generation.get() - COUNT.get()));
                a.is_loading()
                    && move_done(a)
                    && a.status_message().contains("Loading")
                    && !a.list.shows(&Location::dir(p("sub")))
                    && a.generation.get() == COUNT.get() + 2
            },
        );
        s.add(
            "...and a refresh asked for meanwhile waits for it, reading nothing beside it (4.2)",
            |a| {
                COUNT.set(a.generation.get());
                a.queue_refresh();
            },
            |a| {
                note(format!("{} read(s)", a.generation.get() - COUNT.get()));
                // 0.8 s of nothing (the refresh tries every 400 ms).
                step_ms() >= 800
                    && a.refresh_queued.get()
                    && a.generation.get() == COUNT.get()
                    && a.status_message().contains("Loading")
            },
        )
        .during_load();
        s.add(
            "...and once that read is back, the tab shows it with the move's error, then the refresh reads it (4.2)",
            |a| {
                set_text(a.address, "missing tw"); // as if the user were fixing what they typed (5.3)
                read_back(a, None, false);
                // Now: a slow refresh's "Loading..." may take its place.
                FLAG.set(a.status_message().contains("missing two"));
            },
            |a| {
                note(format!("{} read(s)", a.generation.get() - COUNT.get()));
                FLAG.get()
                    && a.idle()
                    && a.list.shows(&Location::dir(p("sub")))
                    && !a.refresh_queued.get()
                    && a.generation.get() == COUNT.get() + 1
                    && text_of(a.address) == "missing tw"
            },
        )
        .during_load();
        // What the status bar says after the error stays: "Loading..." gives back what it took the place of.
        s.first_tab();
        s.add(
            "(a failed move overtakes the switch while its read is still out)",
            |a| overtake(a, "missing three"),
            |a| a.is_loading() && move_done(a) && a.status_message().contains("Loading"),
        );
        s.add(
            "a message shown while the tab waits for its switch's read stays once the folder is back (4.2)",
            |a| {
                a.set_message("a newer message");
                read_back(a, None, false);
            },
            |a| a.idle() && a.list.shows(&Location::dir(p("sub"))) && a.status_message().contains("a newer message"),
        )
        .during_load();
        s.first_tab();
        s.add(
            "(a failed move overtakes the switch while its read is still out, again)",
            |a| overtake(a, "missing four"),
            |a| a.is_loading() && move_done(a),
        );
        s.add(
            "...and a message \"Loading...\" took the place of while it waited comes back with the folder (4.2)",
            |a| {
                a.set_message("a newer message");
                send(a.wnd, WM_TIMER, TIMER_LOADING, 0); // as if the read took 250 ms more
                read_back(a, None, false);
            },
            |a| a.idle() && a.list.shows(&Location::dir(p("sub"))) && a.status_message().contains("a newer message"),
        )
        .during_load();
        // A read that comes back with an error has its answer: the folder is not available (4.3), no second read.
        s.first_tab();
        s.add(
            "(a failed move overtakes the switch while its read is still out, once more)",
            |a| overtake(a, "missing five"),
            |a| a.is_loading() && move_done(a),
        );
        s.add(
            "...and if that read comes back with an error, the tab is not available at once (4.3)",
            |a| {
                COUNT.set(a.generation.get());
                set_text(a.address, "missing fiv"); // as if the user were fixing what they typed (5.3)
                // What a share that is off answers in the end.
                read_back(a, Some(ERROR_BAD_NETPATH.to_hresult().into()), false);
            },
            |a| {
                note(format!("{} read(s)", a.generation.get() - COUNT.get()));
                !a.is_loading()
                    && a.generation.get() == COUNT.get()
                    && a.unavailable()
                    && a.status_message().contains("is not available")
                    && text_of(a.address) == "missing fiv"
            },
        )
        .during_load();
        s.add(
            "(F5 shows it again)",
            |a| {
                a.refresh(ViewState::default());
            },
            |a| a.idle() && a.list.shows(&Location::dir(p("sub"))),
        );
        // A move that lands after the switch's read was shown goes on from the tab's folder, as from any other.
        s.first_tab();
        s.add(
            "a move that lands after the switch's read was shown goes on from the tab's folder (4.2)",
            |a| {
                overtake(a, "big");
                read_back(a, None, false); // the move is still out
            },
            |a| {
                let came_from_sub = a.with_tab(|t| t.back.last().is_some_and(|l| l.same(&Location::dir(p("sub")))));
                a.at(&p("big")) && a.active() == 1 && came_from_sub
            },
        );
        // A change the switch's read saw is about the tab's folder, as one the watch sees: the move's folder is not
        // read again for it (4.5).
        s.first_tab();
        s.add(
            "...and a change seen by the switch's read is not read again in the move's folder (4.5)",
            |a| {
                COUNT.set(a.generation.get());
                overtake(a, "big");
                read_back(a, None, true);
            },
            |a| {
                a.at(&p("big")) && a.active() == 1 && !a.refresh_queued.get() && a.generation.get() == COUNT.get() + 2
            },
        );
        // The move's own hint, said once it lands (5.3), is still to come when the switch's read fails before it.
        s.first_tab();
        s.add(
            "a hint of the move waits for it when the switch's read fails first (5.3)",
            |a| {
                overtake(a, "sub");
                a.late_hint.set(Some("the move's own hint"));
                read_back(a, Some(ERROR_BAD_NETPATH.to_hresult().into()), false); // the move is still out
            },
            |a| a.at(&p("sub")) && a.active() == 1 && a.status_message().contains("the move's own hint"),
        );
        s.add("(close that tab)", |a| a.close_tab(1), one_tab_home);
        // A refresh a move replaced goes with the move: if the move fails, the refresh is asked for again (4.5).
        s.add(
            "a refresh that a failed move replaced is asked for again (4.5)",
            |a| {
                COUNT.set(a.generation.get());
                a.refresh(ViewState::default());
                a.open(Location::dir(p("missing six")), Nav::New, ViewState::default());
            },
            |a| a.idle() && !a.refresh_queued.get() && a.generation.get() == COUNT.get() + 3,
        );
        s.add(
            "(back into big)",
            |a| a.open(Location::dir(p("big")), Nav::New, ViewState::default()),
            |a| a.at(&p("big")),
        );

        s.add(
            "copies 10,000 selected files to the clipboard",
            |a| {
                a.list.select_all();
                let t = Instant::now();
                copy_to_clipboard(a.wnd, &a.list.location(), &a.list.selected_items(), false);
                KEY_MS.set(t.elapsed().as_millis());
                note(format!("copy {} ms", KEY_MS.get()));
            },
            |_| clipboard_files().len() == 10000 && KEY_MS.get() < 6000, // 4.7: under 3 s, with a margin
        )
        .copies();
        s.add(
            "copying takes the files from the list, not from the disk",
            |a| {
                show_ghosts(a);
                copy_to_clipboard(a.wnd, &a.list.location(), &a.list.selected_items(), false);
            },
            |_| clipboard_files().len() == 3,
        )
        .copies();
        // Another thread holds the clipboard open, as a clipboard manager or a remote desktop can. It waits its turn:
        // other programs read the clipboard after each copy, and the files copied above come from this window, so it
        // tries again until it has the clipboard, while the window goes on answering between steps. It opens it with
        // a window of its own: held with no window, the clipboard also opens for any program that asks with none, and
        // that program's close frees it.
        let window = Arc::new(AtomicIsize::new(0)); // its window, while it holds the clipboard
        let tries = Arc::new(AtomicUsize::new(0));
        let release = Rc::new(Cell::new(None::<std::sync::mpsc::Sender<()>>));
        s.add(
            "(another thread takes the clipboard and keeps it open)",
            {
                let (window, tries, release) = (window.clone(), tries.clone(), release.clone());
                move |_| {
                    let (tx, release_wait) = std::sync::mpsc::channel::<()>();
                    release.set(Some(tx));
                    let (window, tries) = (window.clone(), tries.clone());
                    std::thread::spawn(move || {
                        // A message-only window: never shown, and destroyed with the thread.
                        let Ok(w) = (unsafe {
                            CreateWindowExW(
                                WINDOW_EX_STYLE(0),
                                w!("STATIC"),
                                w!(""),
                                WINDOW_STYLE(0),
                                0,
                                0,
                                0,
                                0,
                                Some(HWND_MESSAGE),
                                None,
                                None,
                                None,
                            )
                        }) else {
                            return;
                        };
                        let end = Instant::now() + Duration::from_secs(4);
                        loop {
                            tries.fetch_add(1, Ordering::SeqCst);
                            if unsafe { OpenClipboard(Some(w)) }.is_ok() {
                                break;
                            }
                            if Instant::now() > end {
                                return;
                            }
                            std::thread::sleep(Duration::from_millis(10));
                        }
                        window.store(w.0 as isize, Ordering::SeqCst);
                        let _ = release_wait.recv_timeout(Duration::from_secs(10));
                        unsafe {
                            let _ = CloseClipboard();
                        }
                        window.store(0, Ordering::SeqCst);
                    });
                }
            },
            {
                let (window, tries) = (window.clone(), tries.clone());
                move |_| {
                    note(format!("{} try(s)", tries.load(Ordering::SeqCst)));
                    window.load(Ordering::SeqCst) != 0
                }
            },
        );
        s.add(
            "a Ctrl+C that cannot reach the clipboard says so, not \"copied\" (another program holds it open)",
            move |a| {
                // Selected again and read at once: a refresh in between would replace the files or clear the message.
                show_ghosts(a);
                a.set_message("");
                focus(a.list.hwnd());
                let w = window.load(Ordering::SeqCst);
                if w != 0 && unsafe { GetOpenClipboardWindow() }.is_ok_and(|h| h.0 as isize == w) {
                    a.press(letter(b'C'), CTRL);
                    note(format!("status: {}", a.status_message()));
                } else {
                    note("the test's hold was gone before the key press".into()); // no status: the check fails
                }
                drop(release.take()); // the holder lets go
                while window.load(Ordering::SeqCst) != 0 {
                    std::thread::sleep(Duration::from_millis(1)); // free again for the steps after this one
                }
            },
            |_| {
                let status = noted();
                !status.contains("copied") && status.contains("clipboard")
            },
        );
        // 5.8: Open in new window starts this exe on the folder, a title bar down and right of this window. A start
        // would show a window, which the self-test never does: it checks what would be started.
        let name = "Open in new window starts Qoqdir on the folder, a title bar down and right of this window (5.8)";
        s.check(name, move |a| {
            let w = a.new_window(&dir());
            let words = command_line_words(&w.command_line);
            let exe = Location::dir(std::env::current_exe().unwrap_or_default());
            let named = Location::dir(w.exe.clone()).same(&exe);
            let started = named && words.len() == 2 && Location::dir(PathBuf::from(&words[0])).same(&exe) && {
                let folders = parse_command_line(&words[1..]).folders;
                folders.len() == 1 && folders[0].same(&Location::dir(dir()))
            };
            let r = window_rect(a.wnd);
            let mut mi = MONITORINFO { cbSize: size_of::<MONITORINFO>() as u32, ..Default::default() };
            let _ = unsafe { GetMonitorInfoW(MonitorFromWindow(a.wnd, MONITOR_DEFAULTTONEAREST), &mut mi) };
            let (dx, dy) = (w.place.left - r.left, w.place.top - r.top);
            let moved = (dx > 0 && dx == dy) || (w.place.left == mi.rcWork.left && w.place.top == mi.rcWork.top);
            let same_size =
                w.place.right - w.place.left == r.right - r.left && w.place.bottom - w.place.top == r.bottom - r.top;
            started && moved && same_size && !w.maximized
        });
        // The same for Ctrl+` (5.3): what it would start, and in which folder. The key itself is checked by hand.
        let name = "Ctrl+` would start Windows Terminal (or cmd where it isn't installed) in the folder shown (5.3)";
        s.add(name, move |a| a.open(Location::dir(dir()), Nav::New, ViewState::default()), move |a| {
            if !a.at(&dir()) {
                return false;
            }
            let program = |t: &Terminal| String::from_utf16_lossy(&t.program).trim_end_matches('\0').to_string();
            let t = terminal_program();
            let wt = command_program(OsStr::new("wt"));
            // The name only: the path of wt.exe has the user name in it.
            let leaf = |t: &Terminal| program(t).rsplit('\\').next().unwrap_or_default().to_string();
            note(t.as_ref().map_or("(none)".into(), leaf));
            let right = t.as_ref().is_some_and(|t| match &wt {
                Some(wt) => t.program == *wt && t.args == "-d .",
                None => program(t).to_lowercase().ends_with("\\cmd.exe") && t.args.is_empty(),
            });
            right && Location::dir(a.terminal_folder()).same(&Location::dir(dir()))
        });
        s.add(
            "(on This PC, in the home folder)",
            |a| a.open(Location::ThisPc, Nav::New, ViewState::default()),
            |a| {
                let home = known_folder(&FOLDERID_Profile).map(Location::dir);
                a.at_pc() && home.is_some_and(|home| Location::dir(a.terminal_folder()).same(&home))
            },
        );
        s.home("(back to the test folder)");
        // Long use leaves nothing behind (4.10): opening folders and tabs again and again doesn't pile up handles, GDI
        // or USER objects. A first round fills what is meant to stay (the system image list, the type names).
        let usage = || unsafe {
            let mut handles = 0u32;
            let _ = GetProcessHandleCount(GetCurrentProcess(), &mut handles);
            let gui = |kind| GetGuiResources(GetCurrentProcess(), kind);
            [handles, gui(GR_GDIOBJECTS), gui(GR_USEROBJECTS)]
        };
        // Rounds of four moves: to sub, back to the test folder, a tab on made, closed again. Each waits for the one
        // before to land; true once that many rounds are done.
        let rounds = move |a: &App, n: usize| {
            let tabs_now = if (MOVES.get() + 3) % 4 == 2 { 2 } else { 1 }; // after the newest move
            if !a.idle() || a.tabs() != tabs_now {
                return false;
            }
            let m = MOVES.get();
            if m == n * 4 {
                return true;
            }
            MOVES.set(m + 1);
            match m % 4 {
                0 => a.open(Location::dir(p("sub")), Nav::New, ViewState::default()),
                1 => a.open(Location::dir(dir()), Nav::New, ViewState::default()),
                2 => a.new_tab(Location::dir(p("made")), true),
                _ => a.close_tab(a.active()),
            }
            false
        };
        s.add("(a first round of folders and a tab)", |_| MOVES.set(0), move |a| rounds(a, 1)).wait(20000);
        s.add("(what the process holds then, once its workers are done)", |_| {}, move |_| {
            USAGE.set(usage());
            step_ms() >= 1000
        });
        s.add("(25 rounds more)", |_| MOVES.set(0), move |a| rounds(a, 25)).wait(120000);
        s.add(
            "opening folders and tabs again and again leaves no handles, GDI or USER objects behind (4.10)",
            |_| {},
            move |_| {
                let (was, now) = (USAGE.get(), usage());
                note(format!(
                    "handles {} -> {}, GDI {} -> {}, USER {} -> {}",
                    was[0], now[0], was[1], now[1], was[2], now[2]
                ));
                // A leak of one a round would be 25 more; the margin is for workers still ending.
                step_ms() >= 1000 && now[0] <= was[0] + 15 && now[1] <= was[1] + 4 && now[2] <= was[2] + 4
            },
        );
        // 4.10: a window that answers nothing for 5 s gets a line in the crash log saying where its thread is. Last: a
        // hung window gets a ghost window from Windows meanwhile, which can take the focus from the steps after it.
        // The test's own long steps (making and removing 10,000 files) leave such lines too: a new one must appear.
        let crash_log = move || std::fs::read_to_string(crate::crashlog::log_path(&dir())).unwrap_or_default();
        s.add(
            "a window thread stuck for 7 s leaves a line in the crash log naming where it is",
            move |_| {
                CRASH_LOG_LEN.set(crash_log().len());
                std::thread::sleep(Duration::from_secs(7));
            },
            move |_| {
                let added = crash_log().get(CRASH_LOG_LEN.get()..).unwrap_or("").to_string();
                added.contains("Not responding") && added.contains("exe+0x")
            },
        )
        .wait(15000);

        RUN.set(Some(SelfTest { steps: s.0, index: 0, started: None, held: None, busy: false, failures: 0 }));
        self.quiet.set(true); // the steps trigger errors on purpose: no beeps
        // Its own timer callback, so the app's code doesn't mention the self-test (the exe you use has none).
        unsafe {
            SetTimer(Some(self.wnd), TIMER_SELFTEST, 50, Some(tick));
        }
    }

    fn self_test_tick(&self) {
        enum Next {
            Wait,
            Held, // a folder load still out holds the next step back
            Act(Option<Act>),
            Check(Check, u128, Option<Act>), // the act again, for a step that copies
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
                if self.is_loading() && !step.while_loading { Next::Held } else { Next::Act(step.act.clone()) }
            } else {
                let s = &t.steps[t.index];
                Next::Check(s.done.clone(), s.timeout_ms, if s.copies { s.act.clone() } else { None })
            })
        });
        let Some(next) = next else { return };
        match next {
            Next::Wait => {}
            Next::Held => {
                // A load that never ends would hold back every step after it: after a minute the run fails instead.
                let held = with_test(|t| *t.held.get_or_insert_with(Instant::now));
                if held.elapsed() >= Duration::from_secs(60) {
                    let name = with_test(|t| {
                        t.failures += 1;
                        let name = t.steps[t.index].name;
                        t.index = t.steps.len();
                        name
                    });
                    log(&format!("FAIL  {name}   (not started: a folder load never ended)\n"));
                }
            }
            Next::Act(act) => {
                note(String::new());
                with_test(|t| {
                    t.held = None;
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
                self.self_test_tick(); // checked at once, not on the next tick: a step done right away costs no wait
            }
            Next::Check(done, timeout, again) => {
                let ok = done(self);
                let elapsed = step_ms();
                if !ok && elapsed < timeout {
                    // The clipboard is shared with every program: the user's own copying may have taken it
                    // since. Copy again.
                    if let Some(act) = again {
                        act(self);
                    }
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
                // Files that aren't the test's came in with its paste (a cut made elsewhere just then): they go to a
                // folder of their own next to the test folder before it is deleted, so they can be put back. The test
                // folder itself can't be moved aside: the window still watches folders in it.
                let dir = test_dir();
                let foreign = FOREIGN.with(|f| f.borrow().clone());
                let mut moved = 0;
                if !foreign.is_empty() {
                    let mut kept = None;
                    for n in 1..=99 {
                        let mut name = dir.clone().into_os_string();
                        name.push(if n == 1 { " kept".into() } else { format!(" kept ({n})") });
                        if std::fs::create_dir(&name).is_ok() {
                            kept = Some(PathBuf::from(name));
                            break;
                        }
                    }
                    if let Some(k) = &kept {
                        for name in &foreign {
                            if std::fs::rename(dir.join("sub").join(name), k.join(name)).is_ok() {
                                moved += 1;
                            }
                        }
                    }
                    log(&match kept {
                        Some(k) if moved == foreign.len() => {
                            let k = k.display();
                            format!("files that aren't the test's came in with its paste: they are in {k}\n")
                        }
                        _ => "files that aren't the test's came in with its paste and could not all be moved out: the \
                              test folder is left in place, and the next run deletes it\n"
                            .into(),
                    });
                }
                if moved == foreign.len() {
                    let _ = std::fs::remove_dir_all(&dir);
                }
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
