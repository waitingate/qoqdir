//! The main window: toolbar, tabs, tree, file list and status bar.
//!
//! Threading rule: all state is touched only by the UI thread. Folders are read
//! on short-lived worker threads that own nothing shared; each result is posted
//! back tagged with a generation number, and anything but the newest request
//! is dropped. Navigation only changes state once a folder has loaded, so a
//! failed or slow load can never leave a tab half-updated.
//!
//! Borrow rule: `st` is only borrowed for short, local reads and writes, never
//! across a call into Windows (which can call straight back into `on_message`).

use std::cell::{Cell, RefCell};
use std::ffi::{OsStr, OsString};
use std::path::PathBuf;
use std::rc::{Rc, Weak};

use windows::Win32::Foundation::{COLORREF, HINSTANCE, HWND, LPARAM, LRESULT, POINT, RECT, WPARAM};
use windows::Win32::Graphics::Gdi::{
    ClientToScreen, CreateFontIndirectW, CreateSolidBrush, InvalidateRect, GetDC, GetTextMetricsW, ReleaseDC, ScreenToClient, SelectObject, TEXTMETRICW,
};
use windows::Win32::Storage::FileSystem::CreateDirectoryW;
use windows::Win32::System::Diagnostics::Debug::MessageBeep;
use windows::Win32::System::Com::{COINIT_APARTMENTTHREADED, COINIT_DISABLE_OLE1DDE, CoInitializeEx, CoUninitialize};
use windows::Win32::System::LibraryLoader::GetModuleFileNameW;
use windows::Win32::System::Ole::{IDropTarget, RegisterDragDrop, RevokeDragDrop};
use windows::Win32::System::WindowsProgramming::{GetPrivateProfileStringW, WritePrivateProfileStringW};
use windows::Win32::UI::Controls::{
    EM_SETCUEBANNER, EM_SETSEL, LVIR_LABEL, LVIS_SELECTED, LVITEMW, LVM_CANCELEDITLABEL,
    LVHITTESTINFO, LVM_GETITEMRECT, LVM_HITTEST, LVM_SETITEMSTATE, LVN_BEGINDRAG, LVN_BEGINRDRAG, LVN_ENDLABELEDITW, LVN_ITEMACTIVATE,
    LVM_SETBKCOLOR, LVM_SETTEXTBKCOLOR, TVM_SETBKCOLOR, LVN_ITEMCHANGED, LVN_ODSTATECHANGED, NM_RCLICK, NMHDR, NMLVDISPINFOW, SB_SETPARTS, SB_SETTEXTW,
    TCHITTESTINFO, TCIF_TEXT, TCITEMW, TCM_DELETEALLITEMS, TCM_GETCURSEL, TCM_HITTEST, TCM_INSERTITEMW,
    TCM_HIGHLIGHTITEM, TCM_SETCURSEL, TCN_SELCHANGE,
};
use windows::Win32::UI::HiDpi::{GetDpiForSystem, GetDpiForWindow, SystemParametersInfoForDpi};
use windows::Win32::UI::Input::KeyboardAndMouse::{
    EnableWindow, GetFocus, GetKeyState, ReleaseCapture, SetCapture, SetFocus, VIRTUAL_KEY, VK_BACK, VK_CONTROL, VK_DELETE,
    VK_DOWN, VK_ESCAPE, VK_F2, VK_F3, VK_F4, VK_F5, VK_LEFT, VK_MENU, VK_RETURN, VK_RIGHT, VK_SHIFT, VK_TAB, VK_UP,
};
use windows::Win32::UI::Shell::{DefSubclassProc, FOLDERID_Profile, SHACF_FILESYS_DIRS, SHAutoComplete, SetWindowSubclass};
use windows::Win32::UI::WindowsAndMessaging::{
    AppendMenuW, CW_USEDEFAULT, CreatePopupMenu, CreateWindowExW, DefWindowProcW, DestroyWindow,
    GWLP_USERDATA, GetClientRect, GetCursorPos, GetParent, GetWindowLongPtrW, GetWindowPlacement, GetWindowRect,
    HMENU, HTCLIENT, IDC_ARROW, IDC_SIZEWE, IsChild, KillTimer, WPF_RESTORETOMAXIMIZED, LoadCursorW, MB_ICONQUESTION,
    MB_ICONWARNING, MB_OKCANCEL, MF_GRAYED, MF_STRING, MINMAXINFO, MSG, MessageBoxW, MoveWindow,
    NONCLIENTMETRICSW, PostMessageW, PostQuitMessage, RegisterClassExW, SPI_GETNONCLIENTMETRICS, SW_SHOWMAXIMIZED,
    SW_SHOWNOACTIVATE, SW_SHOWNORMAL, SWP_NOACTIVATE, SWP_NOZORDER, SetCursor, SetTimer,
    SetWindowLongPtrW, SetWindowPlacement, SetWindowPos, SetWindowTextW, ShowWindow, TPM_RETURNCMD,
    TPM_RIGHTBUTTON, TrackPopupMenuEx, WINDOW_EX_STYLE, WINDOW_STYLE, WINDOWPLACEMENT, WM_ACTIVATE, WM_APP,
    WM_APPCOMMAND, WM_CAPTURECHANGED, WM_CHAR, WM_CLOSE, WM_COMMAND, WM_CONTEXTMENU, WM_DESTROY, WM_DEVICECHANGE,
    WM_DPICHANGED, WM_DRAWITEM, WM_GETMINMAXINFO, WM_INITMENUPOPUP, WM_KEYDOWN, WM_LBUTTONDOWN, WM_LBUTTONUP,
    WM_MBUTTONUP, WM_MEASUREITEM, WM_MENUCHAR, WM_MOUSEMOVE, WM_NOTIFY, WM_SETCURSOR, WM_SETFONT, WM_SIZE,
    WM_SYSKEYDOWN, WM_TIMER, WNDCLASSEXW, WS_CHILD, WS_CLIPCHILDREN, WS_EX_CLIENTEDGE, WS_OVERLAPPEDWINDOW,
    WS_TABSTOP, WS_VISIBLE,
};
use windows::core::{PCWSTR, PWSTR, w};

use crate::filelist::FileList;
use crate::APP_NAME;
use crate::folder::{CommandLine, Listing, Location, SortKey, byte_size, extension_of, is_shortcut, list_folder, normalize};
use crate::selftest::SelfTest;
use crate::shell::{
    FolderWatch, IconBatch, acknowledge_change, has_own_icon, read_own_icons, remember_icons, background_menu, copy_to_clipboard, delete_items, drag_out,
    shortcut_target, forward_menu_message, invoke_verb, item_menu, known_folder, new_drop_target, open_file,
    paste_into, rename_item, run_command, show_menu,
};
use crate::tree::{FolderTree, TreeBatch};
use crate::util::{send, Font, OwnedMenu, from_wide, pcwstr, scale, wide, window_text};

const ID_BACK: i32 = 100;
const ID_FORWARD: i32 = 101;
const ID_UP: i32 = 102;
const ID_ADDRESS: i32 = 103;
const ID_FILTER: i32 = 104;
const ID_TABS: i32 = 105;
const ID_PLUS: i32 = 106;
const ID_TREE: i32 = 107;
const ID_LIST: i32 = 108;
const ID_STATUS: i32 = 109;

const WM_APP_LOADED: u32 = WM_APP + 1;
const WM_APP_CHANGED: u32 = WM_APP + 2;
const WM_APP_CLOSETAB: u32 = WM_APP + 3;
const WM_APP_RENAME: u32 = WM_APP + 4;
const WM_APP_ICONS: u32 = WM_APP + 5;
const WM_APP_TREE: u32 = WM_APP + 6;
const WM_APP_MIDDLECLICK: u32 = WM_APP + 7;

pub const TIMER_REFRESH: usize = 1;
const TIMER_STATUS: usize = 2;
pub const TIMER_LOADING: usize = 3;
pub const TIMER_SELFTEST: usize = 50;

const MAX_HISTORY: usize = 100;
/// Soft gray (#EBEBEB): less glare than white.
const BACKGROUND: COLORREF = COLORREF(0x00EBEBEB);
const EN_CHANGE: u32 = 0x0300;
const BN_CLICKED: u32 = 0;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Nav {
    New,
    Back,
    Forward,
    Refresh,
    Switch,
}

/// What to select and scroll to after a folder loads.
#[derive(Clone, Default, Debug)]
pub struct ViewState {
    pub select: Vec<OsString>,
    pub focus: Option<OsString>,
    pub top: Option<usize>,
    pub rename: bool, // start renaming `focus`
}

impl ViewState {
    fn selecting(name: OsString) -> ViewState {
        ViewState { select: vec![name.clone()], focus: Some(name), ..Default::default() }
    }
}

pub struct Tab {
    pub id: u32, // stable identity; indexes shift when tabs close
    pub location: Location,
    pub back: Vec<Location>,
    pub forward: Vec<Location>,
    view: ViewState,   // remembered while the tab is in the background
    unavailable: bool, // its folder failed to load: no drops onto it until it loads again
}

/// Where a load was started from, when that changes what happens if it is not a folder.
#[derive(Clone, Default)]
pub enum Origin {
    #[default]
    Other,
    AddressBar,        // typed in the address bar: a file is opened instead
    Shortcut(PathBuf), // opened from this shortcut: if it leads to a file, Windows opens the shortcut
}

struct Pending {
    generation: u32,
    location: Location,
    nav: Nav,
    view: ViewState,
    tab_id: u32,
    origin: Origin,
}

pub struct State {
    pub tabs: Vec<Tab>,
    pub active: usize,
    next_tab_id: u32,
    pending: Option<Pending>,
    rename: Option<(PathBuf, OsString)>, // done after the edit box has closed
    watch: Option<FolderWatch>,
    last_focus: Option<HWND>,
}

/// A key press, with the modifier keys spelled out (so tests can supply them).
#[derive(Clone, Copy)]
pub struct Key {
    pub message: u32, // WM_KEYDOWN, WM_SYSKEYDOWN or WM_CHAR
    pub code: usize,  // virtual key, or the character
    pub focus: HWND,
    pub ctrl: bool,
    pub shift: bool,
    pub alt: bool,
}

pub struct App {
    pub wnd: HWND,
    back: HWND,
    forward: HWND,
    up: HWND,
    pub address: HWND,
    pub filter: HWND,
    pub tab_bar: HWND,
    plus: HWND,
    pub status: HWND,
    pub list: FileList,
    pub tree: FolderTree,
    pub tab_drop: RefCell<Option<IDropTarget>>, // files dropped on a tab go to its folder
    pub drop_tab: Cell<Option<usize>>,          // tab highlighted during a drag
    font: RefCell<Option<Font>>,
    dpi: Cell<u32>,
    row_height: Cell<i32>,
    tab_height: Cell<i32>,
    tree_width: Cell<i32>, // in 96-DPI units
    splitter_x: Cell<i32>,
    dragging: Cell<bool>,       // moving the tree/list splitter
    dragging_files: Cell<bool>, // a drag started from our own list is in progress
    refresh_queued: Cell<bool>,
    load_message_shown: Cell<bool>, // "Loading..." or "not available": cleared when a folder loads
    show_hidden: Cell<bool>,
    pub generation: Cell<u32>,
    use_settings: bool,
    ini: Vec<u16>, // settings file
    pub st: RefCell<State>,
    pub selftest: RefCell<Option<SelfTest>>,
    pub exit_code: Cell<i32>,
}


fn loword(v: usize) -> u32 {
    (v & 0xFFFF) as u32
}
fn hiword(v: usize) -> u32 {
    ((v >> 16) & 0xFFFF) as u32
}
fn point_from(lp: LPARAM) -> POINT {
    POINT { x: (lp.0 & 0xFFFF) as i16 as i32, y: ((lp.0 >> 16) & 0xFFFF) as i16 as i32 }
}
fn key_down(vk: VIRTUAL_KEY) -> bool {
    unsafe { GetKeyState(vk.0 as i32) < 0 }
}
fn set_text(h: HWND, text: impl AsRef<OsStr>) {
    let w = wide(text);
    unsafe {
        let _ = SetWindowTextW(h, pcwstr(&w));
    }
}

/// Middle-click closes a tab.
unsafe extern "system" fn tab_bar_proc(h: HWND, msg: u32, wp: WPARAM, lp: LPARAM, _: usize, _: usize) -> LRESULT {
    if msg == WM_MBUTTONUP {
        let mut hit = TCHITTESTINFO { pt: point_from(lp), ..Default::default() };
        let index = send(h, TCM_HITTEST, 0, &mut hit as *mut _ as isize);
        if index >= 0 {
            unsafe {
                if let Ok(parent) = GetParent(h) {
                    let _ = PostMessageW(Some(parent), WM_APP_CLOSETAB, WPARAM(index as usize), LPARAM(0));
                }
            }
        }
        return LRESULT(0);
    }
    unsafe { DefSubclassProc(h, msg, wp, lp) }
}

/// Middle-click in the file list or the tree: tell the main window where (screen coordinates).
unsafe extern "system" fn middle_click_proc(h: HWND, msg: u32, wp: WPARAM, lp: LPARAM, _: usize, _: usize) -> LRESULT {
    if msg == WM_MBUTTONUP {
        let mut pt = point_from(lp);
        unsafe {
            let _ = ClientToScreen(h, &mut pt);
            if let Ok(parent) = GetParent(h) {
                let packed = (pt.x as u16 as u32 | ((pt.y as u16 as u32) << 16)) as isize;
                let _ = PostMessageW(Some(parent), WM_APP_MIDDLECLICK, WPARAM(h.0 as usize), LPARAM(packed));
            }
        }
        return LRESULT(0);
    }
    unsafe { DefSubclassProc(h, msg, wp, lp) }
}

impl App {
    // ------------------------------------------------------------ setup

    /// Opens the command line's folders as tabs (or the home folder).
    pub fn create(instance: HINSTANCE, cmd: CommandLine, use_settings: bool) -> Option<Rc<App>> {
        let name = wide(APP_NAME);
        unsafe {
            let class = WNDCLASSEXW {
                cbSize: size_of::<WNDCLASSEXW>() as u32,
                lpfnWndProc: Some(Self::wndproc),
                hInstance: instance,
                hCursor: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
                hbrBackground: CreateSolidBrush(BACKGROUND), // lives as long as the process
                lpszClassName: pcwstr(&name),
                ..Default::default()
            };
            RegisterClassExW(&class);
            let sys_dpi = GetDpiForSystem();
            let wnd = CreateWindowExW(
                WINDOW_EX_STYLE(0),
                pcwstr(&name),
                pcwstr(&name),
                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                scale(1100, sys_dpi),
                scale(700, sys_dpi),
                None,
                None,
                Some(instance),
                None,
            )
            .ok()?;
            let make = |class: PCWSTR, text: PCWSTR, style: u32, ex: u32, id: i32| {
                CreateWindowExW(
                    WINDOW_EX_STYLE(ex),
                    class,
                    text,
                    WINDOW_STYLE(WS_CHILD.0 | WS_VISIBLE.0 | style),
                    0,
                    0,
                    0,
                    0,
                    Some(wnd),
                    Some(HMENU(id as isize as _)),
                    Some(instance),
                    None,
                )
                .unwrap_or_default()
            };
            const ES_AUTOHSCROLL: u32 = 0x80;
            let back = make(w!("BUTTON"), w!("\u{2190}"), 0, 0, ID_BACK);
            let forward = make(w!("BUTTON"), w!("\u{2192}"), 0, 0, ID_FORWARD);
            let up = make(w!("BUTTON"), w!("\u{2191}"), 0, 0, ID_UP);
            let address = make(w!("EDIT"), w!(""), ES_AUTOHSCROLL | WS_TABSTOP.0, WS_EX_CLIENTEDGE.0, ID_ADDRESS);
            let filter = make(w!("EDIT"), w!(""), ES_AUTOHSCROLL | WS_TABSTOP.0, WS_EX_CLIENTEDGE.0, ID_FILTER);
            const TCS_FOCUSNEVER: u32 = 0x8000;
            // Flat button-style tabs: Windows outlines the selected one, which reads clearly on gray.
            const TCS_BUTTONS: u32 = 0x0100;
            const TCS_FLATBUTTONS: u32 = 0x0008;
            let tab_bar = make(w!("SysTabControl32"), w!(""), TCS_FOCUSNEVER | TCS_BUTTONS | TCS_FLATBUTTONS, 0, ID_TABS);
            let plus = make(w!("BUTTON"), w!("+"), 0, 0, ID_PLUS);
            let exe = {
                let mut buf = [0u16; 32768];
                let n = GetModuleFileNameW(None, &mut buf) as usize;
                PathBuf::from(from_wide(&buf[..n]))
            };
            // Bookmarks are shortcuts in a folder next to the exe (in a self-test: in the test folder).
            let bookmarks = match (use_settings, cmd.folders.first().and_then(|f| f.path())) {
                (false, Some(test_dir)) => test_dir.join("Bookmarks"),
                _ => exe.with_file_name("Bookmarks"),
            };
            let _ = std::fs::create_dir(&bookmarks); // fails harmlessly if it exists
            let tree = FolderTree::create(wnd, ID_TREE, WM_APP_TREE, Location::dir(bookmarks));
            let list = FileList::create(wnd, ID_LIST);
            const SBARS_SIZEGRIP: u32 = 0x100;
            let status = make(w!("msctls_statusbar32"), w!(""), SBARS_SIZEGRIP, 0, ID_STATUS);

            // Without settings (self-test), read from a file that cannot exist: every value is a default.
            let ini = wide(exe.with_file_name(if use_settings { format!("{}.ini", APP_NAME.to_lowercase()) } else { "none?.ini".into() }));

            let app = Rc::new(App {
                wnd,
                back,
                forward,
                up,
                address,
                filter,
                tab_bar,
                plus,
                status,
                list,
                tree,
                tab_drop: RefCell::new(None),
                drop_tab: Cell::new(None),
                font: RefCell::new(None),
                dpi: Cell::new(GetDpiForWindow(wnd)),
                row_height: Cell::new(0),
                tab_height: Cell::new(0),
                tree_width: Cell::new(220),
                splitter_x: Cell::new(0),
                dragging: Cell::new(false),
                dragging_files: Cell::new(false),
                refresh_queued: Cell::new(false),
                load_message_shown: Cell::new(false),
                show_hidden: Cell::new(false),
                generation: Cell::new(0),
                use_settings,
                ini,
                st: RefCell::new(State {
                    tabs: Vec::new(),
                    active: 0,
                    next_tab_id: 1,
                    pending: None,
                    rename: None,
                    watch: None,
                    last_focus: None,
                }),
                selftest: RefCell::new(None),
                exit_code: Cell::new(0),
            });
            SetWindowLongPtrW(wnd, GWLP_USERDATA, Rc::as_ptr(&app) as isize);

            send(filter, EM_SETCUEBANNER, 1, w!("Filter  (just start typing)").0 as isize);
            let _ = SHAutoComplete(address, SHACF_FILESYS_DIRS);
            let _ = SetWindowSubclass(tab_bar, Some(tab_bar_proc), 1, 0);
            let _ = SetWindowSubclass(app.list.hwnd(), Some(middle_click_proc), 1, 0);
            let _ = SetWindowSubclass(app.tree.hwnd(), Some(middle_click_proc), 1, 0);
            // Drops land in the current folder; never in This PC, and never back onto our own drag.
            let weak: Weak<App> = Rc::downgrade(&app);
            let drop = new_drop_target(
                wnd,
                Box::new(move |pt| {
                    let app = weak.upgrade()?;
                    let location = app.list.location();
                    (pt.is_some() && !app.dragging_files.get() && matches!(location, Location::Dir(_))).then_some(location)
                }),
            );
            let _ = RegisterDragDrop(app.list.hwnd(), &drop);
            // Dropping on a tab puts the files in that tab's folder.
            let weak: Weak<App> = Rc::downgrade(&app);
            let tab_drop = new_drop_target(wnd, Box::new(move |pt| weak.upgrade()?.tab_drop_target(pt)));
            let _ = RegisterDragDrop(app.tab_bar, &tab_drop);
            *app.tab_drop.borrow_mut() = Some(tab_drop);

            send(app.list.hwnd(), LVM_SETBKCOLOR, 0, BACKGROUND.0 as isize);
            send(app.list.hwnd(), LVM_SETTEXTBKCOLOR, 0, BACKGROUND.0 as isize);
            send(app.tree.hwnd(), TVM_SETBKCOLOR, 0, BACKGROUND.0 as isize);
            app.apply_font();
            app.tree.fill();
            app.load_settings(&cmd);
            app.layout();
            app.show_tabs();
            let (location, view) = app.with_tab(|t| (t.location.clone(), t.view.clone()));
            app.open(location, Nav::Switch, view);
            app.focus_list();
            Some(app)
        }
    }

    unsafe extern "system" fn wndproc(h: HWND, msg: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
        let app = unsafe { GetWindowLongPtrW(h, GWLP_USERDATA) } as *const App;
        if app.is_null() {
            return unsafe { DefWindowProcW(h, msg, wp, lp) };
        }
        // SAFETY: the pointer is set right after creation and cleared in WM_DESTROY;
        // main() keeps the Rc alive for as long as the window exists.
        unsafe { (*app).on_message(msg, wp, lp) }
    }

    fn with_tab<R>(&self, f: impl FnOnce(&mut Tab) -> R) -> R {
        let mut st = self.st.borrow_mut();
        let active = st.active;
        f(&mut st.tabs[active])
    }

    fn location(&self) -> Location {
        self.with_tab(|t| t.location.clone())
    }

    pub fn is_loading(&self) -> bool {
        self.st.borrow().pending.is_some()
    }

    fn apply_font(&self) {
        let dpi = self.dpi.get();
        unsafe {
            let mut ncm = NONCLIENTMETRICSW { cbSize: size_of::<NONCLIENTMETRICSW>() as u32, ..Default::default() };
            let _ = SystemParametersInfoForDpi(
                SPI_GETNONCLIENTMETRICS.0,
                ncm.cbSize,
                Some(&mut ncm as *mut _ as *mut _),
                0,
                dpi,
            );
            let font = Font(CreateFontIndirectW(&ncm.lfMessageFont));
            for h in [self.back, self.forward, self.up, self.address, self.filter, self.tab_bar, self.plus, self.tree.hwnd(), self.list.hwnd(), self.status] {
                send(h, WM_SETFONT, font.0.0 as usize, 1);
            }
            let dc = GetDC(Some(self.wnd));
            let old = SelectObject(dc, font.0.into());
            let mut tm = TEXTMETRICW::default();
            let _ = GetTextMetricsW(dc, &mut tm);
            SelectObject(dc, old);
            ReleaseDC(Some(self.wnd), dc);
            self.row_height.set(tm.tmHeight + scale(10, dpi));
            self.tab_height.set(tm.tmHeight + scale(12, dpi));
            *self.font.borrow_mut() = Some(font); // the old font is freed only after no control uses it
        }
    }

    fn layout(&self) {
        let dpi = self.dpi.get();
        unsafe {
            let mut rc = RECT::default();
            let _ = GetClientRect(self.wnd, &mut rc);
            let (w, h) = (rc.right, rc.bottom);
            send(self.status, WM_SIZE, 0, 0);
            let mut sr = RECT::default();
            let _ = GetWindowRect(self.status, &mut sr);
            let status_h = sr.bottom - sr.top;
            let parts = [scale(380, dpi), -1];
            send(self.status, SB_SETPARTS, 2, parts.as_ptr() as isize);

            let place = |hwnd: HWND, x: i32, y: i32, cx: i32, cy: i32| {
                let _ = MoveWindow(hwnd, x, y, cx.max(0), cy.max(0), true);
            };
            let pad = scale(4, dpi);
            let row = self.row_height.get();
            let btn = row.max(scale(30, dpi));
            let gap = scale(2, dpi);
            let filter_w = scale(240, dpi).min(w / 3);
            let (mut x, mut y) = (pad, pad);
            for b in [self.back, self.forward, self.up] {
                place(b, x, y, btn, row);
                x += btn + gap;
            }
            x += pad;
            place(self.address, x, y, w - x - pad - filter_w - pad, row);
            place(self.filter, w - pad - filter_w, y, filter_w, row);
            y += row + pad;
            let tab_h = self.tab_height.get();
            place(self.tab_bar, 0, y, w - btn - pad, tab_h);
            place(self.plus, w - btn - pad, y + (tab_h - row) / 2, btn, row);
            y += tab_h;

            let split_w = scale(5, dpi);
            let tree_w = scale(self.tree_width.get(), dpi).clamp(scale(80, dpi), scale(80, dpi).max(w - scale(200, dpi)));
            let body_h = h - status_h - y;
            place(self.tree.hwnd(), 0, y, tree_w, body_h);
            place(self.list.hwnd(), tree_w + split_w, y, w - tree_w - split_w, body_h);
            self.splitter_x.set(tree_w);
        }
    }

    // ------------------------------------------------------------ messages

    fn on_message(&self, msg: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
        let dpi = self.dpi.get();
        unsafe {
            match msg {
                WM_SIZE => {
                    self.layout();
                    return LRESULT(0);
                }
                WM_GETMINMAXINFO => {
                    let mmi = &mut *(lp.0 as *mut MINMAXINFO);
                    mmi.ptMinTrackSize = POINT { x: scale(520, dpi), y: scale(320, dpi) };
                    return LRESULT(0);
                }
                WM_DPICHANGED => {
                    let new_dpi = hiword(wp.0);
                    let widths: Vec<i32> =
                        self.list.column_widths().iter().map(|&cw| (cw as i64 * new_dpi as i64 / dpi as i64) as i32).collect();
                    self.dpi.set(new_dpi);
                    self.apply_font();
                    self.list.set_column_widths(&widths);
                    let r = &*(lp.0 as *const RECT);
                    let _ = SetWindowPos(self.wnd, None, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
                    self.layout(); // the size may not have changed, but the font and row heights did
                    return LRESULT(0);
                }
                WM_ACTIVATE => {
                    if loword(wp.0) == 0 {
                        let focus = GetFocus();
                        self.st.borrow_mut().last_focus = Some(focus);
                    } else {
                        let last = self.st.borrow().last_focus.filter(|&h| IsChild(self.wnd, h).as_bool());
                        let _ = SetFocus(Some(last.unwrap_or(self.list.hwnd())));
                    }
                    return LRESULT(0);
                }
                WM_COMMAND => {
                    let (id, code) = (loword(wp.0) as i32, hiword(wp.0));
                    if id == ID_FILTER && code == EN_CHANGE {
                        self.list.set_filter(&window_text(self.filter));
                        return LRESULT(0);
                    }
                    if code != BN_CLICKED {
                        return LRESULT(0);
                    }
                    match id {
                        ID_BACK => self.go_back(),
                        ID_FORWARD => self.go_forward(),
                        ID_UP => self.go_up(),
                        ID_PLUS => self.new_tab(self.location(), true),
                        _ => {}
                    }
                    self.focus_list();
                    return LRESULT(0);
                }
                WM_NOTIFY => return self.on_notify(lp.0 as *const NMHDR),
                WM_CONTEXTMENU => {
                    self.on_context_menu(HWND(wp.0 as _), point_from(lp));
                    return LRESULT(0);
                }
                WM_INITMENUPOPUP | WM_DRAWITEM | WM_MEASUREITEM | WM_MENUCHAR => {
                    if let Some(r) = forward_menu_message(msg, wp, lp) {
                        return r;
                    }
                }
                WM_APP_LOADED => {
                    let listing = Box::from_raw(lp.0 as *mut Listing);
                    self.on_loaded(*listing, wp.0 as u32);
                    return LRESULT(0);
                }
                WM_APP_CHANGED => {
                    acknowledge_change(wp, lp);
                    // At most one refresh per interval, even while a folder changes nonstop.
                    if !self.refresh_queued.replace(true) {
                        SetTimer(Some(self.wnd), TIMER_REFRESH, 400, None);
                    }
                    return LRESULT(0);
                }
                WM_APP_ICONS => {
                    let batch = Box::from_raw(lp.0 as *mut IconBatch);
                    let visible = self.list.location().same(&batch.dir);
                    remember_icons(*batch);
                    if visible {
                        let _ = InvalidateRect(Some(self.list.hwnd()), None, false);
                    }
                    return LRESULT(0);
                }
                WM_APP_TREE => {
                    self.tree.add_children(*Box::from_raw(lp.0 as *mut TreeBatch));
                    return LRESULT(0);
                }
                WM_APP_MIDDLECLICK => {
                    self.on_middle_click(HWND(wp.0 as _), point_from(lp));
                    return LRESULT(0);
                }
                WM_APP_CLOSETAB => {
                    self.close_tab(wp.0);
                    return LRESULT(0);
                }
                WM_APP_RENAME => {
                    let rename = self.st.borrow_mut().rename.take();
                    if let Some((path, name)) = rename {
                        if rename_item(self.wnd, &path, &name).is_ok() {
                            self.refresh(ViewState::selecting(name));
                        }
                    }
                    return LRESULT(0);
                }
                WM_TIMER => {
                    self.on_timer(wp.0);
                    return LRESULT(0);
                }
                WM_APPCOMMAND => {
                    match hiword(lp.0 as usize) & !0xF000 {
                        1 => self.go_back(),
                        2 => self.go_forward(),
                        3 => self.refresh(ViewState::default()),
                        _ => return DefWindowProcW(self.wnd, msg, wp, lp),
                    }
                    return LRESULT(1);
                }
                WM_DEVICECHANGE => {
                    const DBT_DEVICEARRIVAL: usize = 0x8000;
                    const DBT_DEVICEREMOVECOMPLETE: usize = 0x8004;
                    const DBT_DEVTYP_VOLUME: u32 = 2;
                    let volume = lp.0 != 0 && *((lp.0 as *const u32).add(1)) == DBT_DEVTYP_VOLUME;
                    if (wp.0 == DBT_DEVICEARRIVAL || wp.0 == DBT_DEVICEREMOVECOMPLETE) && volume {
                        self.tree.fill();
                        if matches!(self.location(), Location::ThisPc) && !self.is_loading() {
                            self.refresh(ViewState::default());
                        }
                    }
                    return LRESULT(1);
                }
                WM_SETCURSOR => {
                    if HWND(wp.0 as _) == self.wnd && loword(lp.0 as usize) == HTCLIENT {
                        let mut p = POINT::default();
                        let _ = GetCursorPos(&mut p);
                        let _ = ScreenToClient(self.wnd, &mut p);
                        let sx = self.splitter_x.get();
                        if p.x >= sx && p.x < sx + scale(5, dpi) {
                            SetCursor(LoadCursorW(None, IDC_SIZEWE).ok());
                            return LRESULT(1);
                        }
                    }
                }
                WM_LBUTTONDOWN => {
                    let x = point_from(lp).x;
                    let sx = self.splitter_x.get();
                    if x >= sx && x < sx + scale(5, dpi) {
                        self.dragging.set(true);
                        SetCapture(self.wnd);
                    }
                    return LRESULT(0);
                }
                WM_MOUSEMOVE => {
                    if self.dragging.get() {
                        self.tree_width.set((point_from(lp).x.max(0) as i64 * 96 / dpi as i64) as i32);
                        self.layout();
                    }
                    return LRESULT(0);
                }
                WM_LBUTTONUP => {
                    if self.dragging.get() {
                        let _ = ReleaseCapture();
                    }
                    return LRESULT(0);
                }
                WM_CAPTURECHANGED => {
                    self.dragging.set(false);
                    return LRESULT(0);
                }
                WM_CLOSE => {
                    self.save_settings();
                    let _ = DestroyWindow(self.wnd);
                    return LRESULT(0);
                }
                WM_DESTROY => {
                    self.st.borrow_mut().watch = None;
                    let _ = RevokeDragDrop(self.list.hwnd());
                    let _ = RevokeDragDrop(self.tab_bar);
                    SetWindowLongPtrW(self.wnd, GWLP_USERDATA, 0);
                    PostQuitMessage(0);
                    return LRESULT(0);
                }
                _ => {}
            }
            DefWindowProcW(self.wnd, msg, wp, lp)
        }
    }

    fn on_timer(&self, id: usize) {
        unsafe {
            match id {
                TIMER_REFRESH => {
                    // Otherwise keep the timer and try again: never disturb a rename,
                    // and never cancel a folder that is loading.
                    if !self.list.renaming() && !self.is_loading() {
                        let _ = KillTimer(Some(self.wnd), TIMER_REFRESH);
                        self.refresh_queued.set(false);
                        self.refresh(ViewState::default());
                    }
                }
                TIMER_STATUS => {
                    let _ = KillTimer(Some(self.wnd), TIMER_STATUS);
                    self.show_status();
                }
                TIMER_LOADING => {
                    let _ = KillTimer(Some(self.wnd), TIMER_LOADING);
                    if self.is_loading() {
                        self.set_message("Loading\u{2026}"); // only when it is slow
                        self.load_message_shown.set(true);
                    }
                }
                TIMER_SELFTEST => self.self_test_tick(),
                _ => {}
            }
        }
    }

    /// Middle-click on a folder in the list or the tree opens it in a new tab, in the background.
    fn on_middle_click(&self, from: HWND, pt: POINT) {
        if from == self.list.hwnd() {
            let mut hit = LVHITTESTINFO { pt, ..Default::default() };
            unsafe {
                let _ = ScreenToClient(from, &mut hit.pt);
            }
            let row = send(from, LVM_HITTEST, 0, &mut hit as *mut _ as isize);
            let folder = usize::try_from(row).ok().filter(|&r| self.list.item(r).is_some_and(|i| i.is_dir()));
            if let Some(location) = folder.and_then(|r| self.list.item_location(r)) {
                self.new_tab(location, false);
            }
        } else if from == self.tree.hwnd() {
            if let Some(location) = self.tree.location_at(pt) {
                self.open_node(location, true);
            }
        }
    }

    fn on_notify(&self, nm: *const NMHDR) -> LRESULT {
        let (from, code) = unsafe { ((*nm).hwndFrom, (*nm).code) };
        if from == self.list.hwnd() {
            if let Some(r) = unsafe { self.list.on_notify(nm) } {
                return r;
            }
            match code {
                LVN_ITEMCHANGED | LVN_ODSTATECHANGED => unsafe {
                    SetTimer(Some(self.wnd), TIMER_STATUS, 30, None); // once per burst, not once per item
                },
                LVN_ITEMACTIVATE => self.activate(key_down(VK_CONTROL)),
                LVN_ENDLABELEDITW => {
                    let info = unsafe { &*(nm as *const NMLVDISPINFOW) };
                    let row = info.item.iItem as usize;
                    if !info.item.pszText.is_null() {
                        let name = from_wide(unsafe { info.item.pszText.as_wide() });
                        if let (Some(it), Some(loc)) = (self.list.item(row), self.list.item_location(row)) {
                            if name != it.name && !name.is_empty() {
                                if let Location::Dir(path) = loc {
                                    self.st.borrow_mut().rename = Some((path, name));
                                    unsafe {
                                        let _ = PostMessageW(Some(self.wnd), WM_APP_RENAME, WPARAM(0), LPARAM(0));
                                    }
                                }
                            }
                        }
                    }
                    return LRESULT(0);
                }
                LVN_BEGINDRAG | LVN_BEGINRDRAG => {
                    let items = self.list.selected_items();
                    if !items.is_empty() {
                        self.dragging_files.set(true);
                        drag_out(self.wnd, &items);
                        self.dragging_files.set(false);
                    }
                }
                _ => {}
            }
            return LRESULT(0);
        }
        if from == self.tree.hwnd() {
            if let Some(location) = unsafe { self.tree.on_notify(nm) } {
                self.open_node(location, false);
            }
            return LRESULT(0);
        }
        if from == self.tab_bar {
            if code == TCN_SELCHANGE {
                self.switch_tab(send(self.tab_bar, TCM_GETCURSEL, 0, 0).max(0) as usize);
            }
            if code == NM_RCLICK {
                self.on_tab_menu();
            }
        }
        LRESULT(0)
    }

    fn on_context_menu(&self, from: HWND, mut pt: POINT) {
        let keyboard = pt.x == -1 && pt.y == -1;
        if from == self.list.hwnd() {
            if self.list.renaming() {
                return;
            }
            if keyboard {
                let mut r = RECT { left: LVIR_LABEL as i32, ..Default::default() };
                let row = self.list.focused().unwrap_or(0);
                pt = if send(self.list.hwnd(), LVM_GETITEMRECT, row, &mut r as *mut _ as isize) != 0 {
                    POINT { x: r.left, y: r.bottom }
                } else {
                    POINT::default()
                };
                unsafe {
                    let _ = ClientToScreen(self.list.hwnd(), &mut pt);
                }
            }
            let rows = self.list.selection();
            if rows.is_empty() {
                // The folder's own menu, with Paste; none when the list shows no folder.
                let location = self.list.location();
                if matches!(location, Location::Dir(_))
                    && let Some(cm) = background_menu(self.wnd, &location)
                {
                    show_menu(self.wnd, &cm, pt, &mut |_| false, Some(&|shortcuts| paste_into(self.wnd, &location, shortcuts)));
                }
                return;
            }
            // Folders (and a shortcut's folder) open here, not in a separate Explorer window.
            let opens_here = rows.iter().all(|&r| self.list.item(r).is_some_and(|i| i.is_dir()))
                || (rows.len() == 1 && self.list.item(rows[0]).is_some_and(|i| is_shortcut(&i.name)));
            let Some(cm) = item_menu(self.wnd, &self.list.selected_items()) else { return };
            show_menu(self.wnd, &cm, pt, &mut |verb| {
                let is = |name: &str| verb.eq_ignore_ascii_case(name);
                if is("rename") {
                    self.list.rename(self.list.focused().or(rows.first().copied()));
                    return true;
                }
                if opens_here && (is("open") || is("explore")) {
                    self.activate(false);
                    return true;
                }
                if opens_here && (is("opennewtab") || is("opennewwindow")) {
                    self.activate(true);
                    return true;
                }
                false
            }, None);
            return;
        }
        if from == self.tree.hwnd() {
            let Some(location) = self.tree.location_at(pt) else { return };
            if keyboard {
                let mut r = RECT::default();
                unsafe {
                    let _ = GetWindowRect(self.tree.hwnd(), &mut r);
                }
                pt = POINT { x: r.left + scale(24, self.dpi.get()), y: r.top + scale(24, self.dpi.get()) };
            }
            let Some(cm) = item_menu(self.wnd, std::slice::from_ref(&location)) else { return };
            show_menu(self.wnd, &cm, pt, &mut |verb| {
                let is = |name: &str| verb.eq_ignore_ascii_case(name);
                if is("open") || is("explore") {
                    self.open_node(location.clone(), false);
                } else if is("opennewtab") || is("opennewwindow") {
                    self.open_node(location.clone(), true);
                } else {
                    return is("rename"); // renaming is done in the file list
                }
                true
            }, None);
        }
    }

    fn on_tab_menu(&self) {
        let mut hit = TCHITTESTINFO::default();
        unsafe {
            let _ = GetCursorPos(&mut hit.pt);
        }
        let screen = hit.pt;
        unsafe {
            let _ = ScreenToClient(self.tab_bar, &mut hit.pt);
        }
        let index = send(self.tab_bar, TCM_HITTEST, 0, &mut hit as *mut _ as isize);
        if index < 0 {
            return;
        }
        let index = index as usize;
        let tab_count = self.st.borrow().tabs.len();
        let cmd = unsafe {
            let Ok(menu) = CreatePopupMenu() else { return };
            let menu = OwnedMenu(menu);
            let _ = AppendMenuW(menu.0, MF_STRING, 1, w!("Duplicate tab"));
            let _ = AppendMenuW(menu.0, MF_STRING, 2, w!("Close tab\tCtrl+W"));
            let grayed = if tab_count > 1 { MF_STRING } else { MF_STRING | MF_GRAYED };
            let _ = AppendMenuW(menu.0, grayed, 3, w!("Close other tabs"));
            TrackPopupMenuEx(menu.0, (TPM_RETURNCMD | TPM_RIGHTBUTTON).0, screen.x, screen.y, self.wnd, None).0
        };
        match cmd {
            1 => {
                let location = self.st.borrow().tabs.get(index).map(|t| t.location.clone());
                if let Some(location) = location {
                    self.new_tab(location, true);
                }
            }
            2 => self.close_tab(index),
            3 => self.close_other_tabs(index),
            _ => {}
        }
    }

    // ------------------------------------------------------------ keyboard

    /// Called for every message before dispatch.
    pub fn handle_key(&self, m: &MSG) -> bool {
        if m.message != WM_KEYDOWN && m.message != WM_SYSKEYDOWN && m.message != WM_CHAR {
            return false;
        }
        // Only our own window: never steal keys from dialogs such as Properties.
        if m.hwnd != self.wnd && !unsafe { IsChild(self.wnd, m.hwnd) }.as_bool() {
            return false;
        }
        self.on_key(Key {
            message: m.message,
            code: m.wParam.0,
            focus: unsafe { GetFocus() },
            ctrl: key_down(VK_CONTROL),
            shift: key_down(VK_SHIFT),
            alt: key_down(VK_MENU),
        })
    }

    pub fn on_key(&self, key: Key) -> bool {
        if self.list.renaming() {
            return false; // the rename box handles its own keys
        }
        let (focus, list) = (key.focus, self.list.hwnd());
        let (ctrl, shift, alt) = (key.ctrl, key.shift, key.alt);

        if key.message == WM_CHAR {
            // Typing in the file list filters it.
            if focus == list && key.code >= 32 && key.code != 127 && !ctrl && !alt {
                unsafe {
                    let _ = SetFocus(Some(self.filter));
                }
                let len = window_text(self.filter).len();
                send(self.filter, EM_SETSEL, len, len as isize);
                send(self.filter, WM_CHAR, key.code, 0);
                return true;
            }
            return false;
        }

        let k = key.code as u16;
        let is = |vk: VIRTUAL_KEY| k == vk.0;
        let letter = |c: u8| k == c as u16;
        let tab_count = self.st.borrow().tabs.len();
        let active = self.st.borrow().active;
        let focus_edit = |edit: HWND| {
            unsafe {
                let _ = SetFocus(Some(edit));
            }
            send(edit, EM_SETSEL, 0, -1);
        };

        // Anywhere in the window.
        if ctrl && letter(b'T') {
            self.new_tab(self.location(), true);
            return true;
        }
        if ctrl && letter(b'W') {
            self.close_tab(active);
            return true;
        }
        if ctrl && is(VK_TAB) {
            self.switch_tab((active + if shift { tab_count - 1 } else { 1 }) % tab_count);
            return true;
        }
        if ctrl && (b'1' as u16..=b'9' as u16).contains(&k) {
            let index = if letter(b'9') { tab_count - 1 } else { ((k - b'1' as u16) as usize).min(tab_count - 1) };
            self.switch_tab(index);
            return true;
        }
        if (ctrl && letter(b'L')) || (alt && letter(b'D')) || (is(VK_F4) && !alt) {
            focus_edit(self.address);
            return true;
        }
        if (ctrl && letter(b'F')) || is(VK_F3) {
            focus_edit(self.filter);
            return true;
        }
        if is(VK_F5) || (ctrl && letter(b'R')) {
            self.refresh(ViewState::default());
            return true;
        }
        if ctrl && letter(b'H') {
            let hidden = !self.show_hidden.get();
            self.show_hidden.set(hidden);
            self.set_message(if hidden { "Showing hidden files" } else { "Hiding hidden files" });
            self.reload();
            return true;
        }
        if alt && is(VK_LEFT) {
            self.go_back();
            return true;
        }
        if alt && is(VK_RIGHT) {
            self.go_forward();
            return true;
        }
        if alt && is(VK_UP) {
            self.go_up();
            return true;
        }
        if alt && is(VK_RETURN) && focus == list {
            invoke_verb(self.wnd, item_menu(self.wnd, &self.list.selected_items()), "properties");
            return true;
        }
        if is(VK_TAB) && !ctrl && !alt {
            let order = [self.address, self.filter, self.tree.hwnd(), list];
            let i = order.iter().position(|&h| h == focus).unwrap_or(3);
            let next = order[(i + if shift { 3 } else { 1 }) % 4];
            if next == list {
                self.focus_list();
            } else if next == self.tree.hwnd() {
                unsafe {
                    let _ = SetFocus(Some(next));
                }
            } else {
                focus_edit(next);
            }
            return true;
        }
        if key.message != WM_KEYDOWN {
            return false;
        }

        if focus == self.address {
            if is(VK_RETURN) {
                self.go_to_typed();
                return true;
            }
            if is(VK_ESCAPE) {
                set_text(self.address, self.location().text());
                self.focus_list();
                return true;
            }
            return false;
        }
        if focus == self.filter {
            if is(VK_ESCAPE) {
                set_text(self.filter, "");
                self.focus_list();
                return true;
            }
            if is(VK_DOWN) {
                self.focus_list();
                return true;
            }
            if is(VK_RETURN) {
                self.focus_list();
                self.activate(ctrl);
                return true;
            }
            return false;
        }
        if focus == list {
            if is(VK_RETURN) {
                self.activate(ctrl);
                return true;
            }
            if is(VK_BACK) {
                self.go_up();
                return true;
            }
            if is(VK_DELETE) {
                self.delete_selection(shift);
                return true;
            }
            if is(VK_F2) {
                let rows = self.list.selection();
                if let Some(&first) = rows.first() {
                    self.list.rename(Some(if ctrl { first } else { self.list.focused().unwrap_or(first) }));
                }
                return true;
            }
            if is(VK_ESCAPE) && !window_text(self.filter).is_empty() {
                set_text(self.filter, "");
                return true;
            }
            if ctrl && shift && letter(b'N') {
                self.new_folder();
                return true;
            }
            if ctrl && letter(b'A') {
                self.list.select_all();
                return true;
            }
            if ctrl && (letter(b'C') || letter(b'X')) {
                let items = self.list.selected_items();
                if !items.is_empty() {
                    let cut = letter(b'X');
                    copy_to_clipboard(self.wnd, &items, cut);
                    self.set_message(&format!("{} item(s) {}", items.len(), if cut { "cut" } else { "copied" }));
                }
                return true;
            }
            if ctrl && letter(b'V') {
                paste_into(self.wnd, &self.list.location(), false);
                return true;
            }
            return false;
        }
        if focus == self.tree.hwnd() && is(VK_BACK) {
            self.go_up();
            return true;
        }
        false
    }

    pub fn focus_list(&self) {
        unsafe {
            let _ = SetFocus(Some(self.list.hwnd()));
        }
        if self.list.selection().is_empty() {
            if let Some(row) = self.list.focused() {
                let item = LVITEMW { state: LVIS_SELECTED, stateMask: LVIS_SELECTED, ..Default::default() };
                send(self.list.hwnd(), LVM_SETITEMSTATE, row, &item as *const _ as isize);
            }
        }
    }

    // ------------------------------------------------------------ navigation

    pub fn open(&self, location: Location, nav: Nav, view: ViewState) {
        self.open_from(location, nav, view, Origin::Other);
    }

    fn open_from(&self, location: Location, nav: Nav, view: ViewState, origin: Origin) {
        if self.list.renaming() {
            send(self.list.hwnd(), LVM_CANCELEDITLABEL, 0, 0);
        }
        let generation = self.generation.get() + 1;
        self.generation.set(generation);
        {
            let mut st = self.st.borrow_mut();
            let tab_id = st.tabs[st.active].id;
            st.pending = Some(Pending { generation, location: location.clone(), nav, view, tab_id, origin });
        }
        unsafe {
            SetTimer(Some(self.wnd), TIMER_LOADING, 250, None); // say "Loading..." only if it is slow
        }
        let wnd = self.wnd.0 as usize; // HWND is just a handle; the thread only posts to it
        let hidden = self.show_hidden.get();
        std::thread::spawn(move || unsafe {
            let co = CoInitializeEx(None, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            let listing = list_folder(&location, hidden);
            let own: Vec<OsString> = listing.items.iter().filter(|it| has_own_icon(it)).map(|it| it.name.clone()).collect();
            let result = Box::into_raw(Box::new(listing));
            if PostMessageW(Some(HWND(wnd as _)), WM_APP_LOADED, WPARAM(generation as usize), LPARAM(result as isize)).is_err() {
                drop(Box::from_raw(result)); // the window is gone
            }
            // Then the icons that need a disk (or network) lookup, so they never hold up the list.
            if !own.is_empty() {
                let icons = Box::into_raw(Box::new(read_own_icons(&location, own)));
                if PostMessageW(Some(HWND(wnd as _)), WM_APP_ICONS, WPARAM(0), LPARAM(icons as isize)).is_err() {
                    drop(Box::from_raw(icons));
                }
            }
            if co.is_ok() {
                CoUninitialize();
            }
        });
    }

    fn on_loaded(&self, listing: Listing, generation: u32) {
        if generation != self.generation.get() {
            return; // superseded by a newer request
        }
        let pending = {
            let mut st = self.st.borrow_mut();
            let pending = st.pending.take();
            match pending {
                Some(p) if p.generation == generation && st.tabs[st.active].id == p.tab_id => p,
                _ => return,
            }
        };
        unsafe {
            let _ = KillTimer(Some(self.wnd), TIMER_LOADING);
        }
        if self.load_message_shown.replace(false) {
            self.set_message(""); // any load, not only navigation, may have shown it
        }

        if let Some(error) = &listing.error {
            return self.on_load_failed(&listing, error, &pending);
        }

        let same_folder = self.list.location().same(&listing.location);
        let moved = matches!(pending.nav, Nav::New | Nav::Back | Nav::Forward);
        let mut view = pending.view;
        if pending.nav == Nav::Refresh && same_folder {
            if view.select.is_empty() && view.focus.is_none() {
                view = self.current_view();
            }
            if view.top.is_none() {
                view.top = Some(self.list.top_row());
            }
        }
        let old = self.with_tab(|t| {
            let old = t.location.clone();
            match pending.nav {
                Nav::New if !old.same(&listing.location) => {
                    t.back.push(old.clone());
                    t.forward.clear();
                }
                Nav::Back => {
                    t.back.pop();
                    t.forward.push(old.clone());
                }
                Nav::Forward => {
                    t.forward.pop();
                    t.back.push(old.clone());
                }
                _ => {}
            }
            if t.back.len() > MAX_HISTORY {
                t.back.remove(0);
            }
            if t.forward.len() > MAX_HISTORY {
                t.forward.remove(0);
            }
            t.location = listing.location.clone();
            t.unavailable = false;
            old
        });
        if moved && view.select.is_empty() && view.focus.is_none() {
            // Going up (or back to a parent) selects the folder you came from.
            if let Some(child) = listing.location.child_toward(&old) {
                view = ViewState::selecting(child);
            }
        }
        if !(pending.nav == Nav::Refresh && same_folder) && !window_text(self.filter).is_empty() {
            set_text(self.filter, "");
        }

        let location = listing.location.clone();
        self.list.set_listing(listing);
        let row = self.list.select(&view.select, view.focus.as_deref());
        match view.top {
            Some(top) => self.list.scroll_to_row(top),
            None => self.list.ensure_visible(row),
        }
        if view.rename && row.is_some() {
            self.list.rename(row);
        }

        let watch = FolderWatch::new(self.wnd, WM_APP_CHANGED, &location);
        let old_watch = self.st.borrow_mut().watch.replace(watch);
        drop(old_watch);
        self.show_location(pending.nav);
        if moved {
            self.set_message("");
        }
        self.show_status();
    }

    /// The address bar, title, buttons and tabs follow the current tab's folder.
    fn show_location(&self, nav: Nav) {
        let location = self.location();
        if nav != Nav::Refresh || unsafe { GetFocus() } != self.address {
            set_text(self.address, location.text()); // never overwrite what the user is typing
        }
        let mut title = location.display_name();
        title.push(format!(" - {APP_NAME}"));
        set_text(self.wnd, title);
        let (can_back, can_forward) = self.with_tab(|t| (!t.back.is_empty(), !t.forward.is_empty()));
        unsafe {
            let _ = EnableWindow(self.back, can_back);
            let _ = EnableWindow(self.forward, can_forward);
            let _ = EnableWindow(self.up, matches!(location, Location::Dir(_)));
        }
        self.show_tabs();
    }

    /// The folder could not be read: decide what to show or do instead.
    fn on_load_failed(&self, listing: &Listing, error: &str, pending: &Pending) {
        match (&pending.origin, &listing.location) {
            // The shortcut leads to a file: let Windows open it. (Not when its folder is unreachable:
            // Windows would only show its own error, after the same long wait.)
            (Origin::Shortcut(lnk), _) if listing.is_file => {
                open_file(self.wnd, lnk);
                return;
            }
            (Origin::AddressBar, Location::Dir(path)) if listing.is_file => {
                open_file(self.wnd, path); // a file typed in the address bar: open it
                set_text(self.address, self.location().text());
                return;
            }
            (Origin::AddressBar, _) => {
                // Let the user correct what they typed.
                unsafe {
                    let _ = SetFocus(Some(self.address));
                }
                send(self.address, EM_SETSEL, 0, -1);
            }
            _ => {}
        }
        if matches!(pending.nav, Nav::Refresh | Nav::Switch) {
            return self.show_unavailable(&listing.location, error, pending.nav);
        }
        if matches!(pending.nav, Nav::Back | Nav::Forward) {
            // A history entry whose folder is gone is dropped, so Back/Forward can move past it.
            let (can_back, can_forward) = self.with_tab(|t| {
                let history = if pending.nav == Nav::Back { &mut t.back } else { &mut t.forward };
                if history.last().is_some_and(|l| l.same(&listing.location)) {
                    history.pop();
                }
                (!t.back.is_empty(), !t.forward.is_empty())
            });
            unsafe {
                let _ = EnableWindow(self.back, can_back);
                let _ = EnableWindow(self.forward, can_forward);
            }
        }
        self.set_message(&format!("{}: {}", listing.location.display_name().to_string_lossy(), error.trim()));
        self.warn();
    }

    /// The tab's own folder could not be read. A tab never changes its folder by itself (a network
    /// drive may not be connected yet, a USB drive may come back): show it empty and say why.
    /// The list gets no folder (as for This PC), so paste, new folder, menus and drops have nothing
    /// to act on: asking Windows about an unreachable folder can freeze the window for 20-40 s.
    pub fn show_unavailable(&self, location: &Location, error: &str, nav: Nav) {
        if !window_text(self.filter).is_empty() {
            set_text(self.filter, "");
        }
        self.list.set_listing(Listing { location: Location::ThisPc, items: Vec::new(), error: None, is_file: false });
        self.with_tab(|t| t.unavailable = true);
        let old_watch = self.st.borrow_mut().watch.take();
        drop(old_watch);
        self.show_location(nav);
        self.set_message(&format!("\"{}\" is not available: {}   (F5 tries again)", location.text().to_string_lossy(), error.trim()));
        self.load_message_shown.set(true);
        self.show_status();
    }

    fn go_back(&self) {
        if let Some(target) = self.with_tab(|t| t.back.last().cloned()) {
            self.open(target, Nav::Back, ViewState::default());
        }
    }

    fn go_forward(&self) {
        if let Some(target) = self.with_tab(|t| t.forward.last().cloned()) {
            self.open(target, Nav::Forward, ViewState::default());
        }
    }

    fn go_up(&self) {
        let location = self.location();
        if let Location::Dir(_) = location {
            self.open(location.parent(), Nav::New, ViewState::default());
        }
    }

    pub fn refresh(&self, view: ViewState) {
        // A folder is already loading, and what it shows will be fresh: start nothing new.
        // (Refreshing the old folder would cancel it; restarting it on every held-down F5
        // would pile up stuck threads on a slow network share.)
        if !self.is_loading() {
            self.open(self.location(), Nav::Refresh, view);
        }
    }

    /// Like refresh, but also restarts a load in progress (e.g. after Ctrl+H).
    fn reload(&self) {
        let loading = self.st.borrow().pending.as_ref().map(|p| (p.location.clone(), p.nav, p.view.clone(), p.origin.clone()));
        match loading {
            Some((location, nav, view, origin)) => self.open_from(location, nav, view, origin),
            None => self.refresh(ViewState::default()),
        }
    }

    fn go_to_typed(&self) {
        let text = window_text(self.address);
        let mut location = normalize(&text);
        // ".." and the name of a subfolder here open in Qoqdir (Windows would open them in Explorer).
        let here = self.location();
        if let Location::Dir(typed) = &location {
            if typed.as_os_str() == ".." {
                location = here.parent();
            } else if matches!(here, Location::Dir(_)) && self.list.has_folder(typed.as_os_str()) {
                location = Location::dir(here.join(typed.as_os_str()));
            }
        }
        if location.path().is_none_or(|p| p.is_absolute()) {
            // Checked by the folder loader on its worker thread, so a dead network
            // share cannot freeze the window. A file is opened when the result comes back.
            self.open_from(location, Nav::New, ViewState::default(), Origin::AddressBar);
            self.focus_list();
            return;
        }
        let current = self.location();
        if !run_command(self.wnd, &text, current.path()) {
            // "cmd", "notepad", a web address... none of those either.
            self.set_message(&format!("Can't find \"{}\"", text.to_string_lossy()));
            self.warn();
            return;
        }
        set_text(self.address, current.text());
        self.focus_list();
    }

    /// Enter / double-click on the selection.
    fn activate(&self, in_new_tab: bool) {
        let mut rows = self.list.selection();
        if rows.is_empty() {
            rows.extend(self.list.focused());
        }
        if let [row] = rows.as_slice() {
            if let (Some(it), Some(Location::Dir(lnk))) = (self.list.item(*row), self.list.item_location(*row)) {
                if !it.is_dir() && is_shortcut(&it.name) {
                    self.open_shortcut(lnk, in_new_tab);
                    return;
                }
            }
        }
        let (mut dirs, mut files) = (Vec::new(), Vec::new());
        for row in rows {
            let (Some(it), Some(Location::Dir(path))) = (self.list.item(row), self.list.item_location(row)) else {
                continue;
            };
            if it.is_dir() { dirs.push(Location::dir(path)) } else { files.push(path) }
        }
        if files.len() > 15 {
            let text = wide(format!("Open {} files?", files.len()));
            let answer = unsafe { MessageBoxW(Some(self.wnd), pcwstr(&text), pcwstr(&wide(APP_NAME)), MB_OKCANCEL | MB_ICONQUESTION) };
            if answer.0 != 1 {
                return; // not IDOK
            }
        }
        for f in &files {
            open_file(self.wnd, f);
        }
        if dirs.len() == 1 && !in_new_tab {
            self.open(dirs.remove(0), Nav::New, ViewState::default());
        } else {
            for d in dirs {
                self.new_tab(d, false); // several folders: background tabs
            }
        }
    }

    /// A shortcut that leads to a folder shows that folder here, or in a background tab when the
    /// shortcut says its target is a folder; Windows opens anything else. Whether the target really
    /// is a folder is found out by the folder loader on its worker thread (it may be on a network).
    fn open_shortcut(&self, path: PathBuf, in_new_tab: bool) {
        let lnk = extension_of(path.as_os_str()) == ".lnk";
        let target = if lnk { shortcut_target(&path).filter(|(t, _)| t.is_absolute()) } else { None };
        match target {
            None => open_file(self.wnd, &path), // a web link, a program...
            Some((target, true)) if in_new_tab => self.new_tab(Location::dir(target), false),
            Some((target, _)) => self.open_from(Location::dir(target), Nav::New, ViewState::default(), Origin::Shortcut(path)),
        }
    }

    /// A node in the tree: a folder, or a bookmark shortcut.
    fn open_node(&self, location: Location, in_new_tab: bool) {
        match location {
            Location::Dir(path) if is_shortcut(path.as_os_str()) => self.open_shortcut(path, in_new_tab),
            location if in_new_tab => self.new_tab(location, false),
            location => self.open(location, Nav::New, ViewState::default()),
        }
    }

    fn current_view(&self) -> ViewState {
        if !self.list.location().same(&self.location()) {
            return ViewState::default();
        }
        ViewState { select: self.list.selected_names(), focus: self.list.focused_name(), top: Some(self.list.top_row()), rename: false }
    }

    // ------------------------------------------------------------ tabs

    fn make_tab(st: &mut State, location: Location) -> Tab {
        let id = st.next_tab_id;
        st.next_tab_id += 1;
        Tab { id, location, back: Vec::new(), forward: Vec::new(), view: ViewState::default(), unavailable: false }
    }

    pub fn new_tab(&self, location: Location, switch_to: bool) {
        let view = if switch_to { Some(self.current_view()) } else { None };
        {
            let mut st = self.st.borrow_mut();
            let at = st.active + 1;
            let tab = Self::make_tab(&mut st, location.clone());
            st.tabs.insert(at, tab);
            if let Some(view) = view {
                let active = st.active;
                st.tabs[active].view = view;
                st.active = at;
            }
        }
        if switch_to {
            self.open(location, Nav::Switch, ViewState::default());
        }
        self.show_tabs();
    }

    pub fn close_tab(&self, index: usize) {
        let reopen = {
            let mut st = self.st.borrow_mut();
            if st.tabs.len() <= 1 || index >= st.tabs.len() {
                return;
            }
            let was_active = index == st.active;
            st.tabs.remove(index);
            if index < st.active {
                st.active -= 1;
            }
            st.active = st.active.min(st.tabs.len() - 1);
            let t = &st.tabs[st.active];
            was_active.then(|| (t.location.clone(), t.view.clone()))
        };
        self.show_tabs();
        if let Some((location, view)) = reopen {
            self.open(location, Nav::Switch, view);
        }
    }

    fn close_other_tabs(&self, index: usize) {
        let reopen = {
            let mut st = self.st.borrow_mut();
            if index >= st.tabs.len() {
                return;
            }
            let was_active = index == st.active;
            let keep = st.tabs.swap_remove(index);
            st.tabs.clear();
            st.tabs.push(keep);
            st.active = 0;
            (!was_active).then(|| (st.tabs[0].location.clone(), st.tabs[0].view.clone()))
        };
        self.show_tabs();
        if let Some((location, view)) = reopen {
            self.open(location, Nav::Switch, view);
        }
    }

    fn switch_tab(&self, index: usize) {
        let (count, active) = {
            let st = self.st.borrow();
            (st.tabs.len(), st.active)
        };
        if index >= count || index == active {
            self.show_tabs();
            return;
        }
        let view = self.current_view();
        let (location, view) = {
            let mut st = self.st.borrow_mut();
            st.tabs[active].view = view;
            st.active = index;
            (st.tabs[index].location.clone(), st.tabs[index].view.clone())
        };
        self.show_tabs();
        self.open(location, Nav::Switch, view);
    }

    /// The folder of the tab under the cursor, for a drop. That tab lights up while hovered.
    fn tab_drop_target(&self, pt: Option<POINT>) -> Option<Location> {
        let hover = pt.and_then(|pt| {
            let mut hit = TCHITTESTINFO { pt, ..Default::default() };
            unsafe {
                let _ = ScreenToClient(self.tab_bar, &mut hit.pt);
            }
            usize::try_from(send(self.tab_bar, TCM_HITTEST, 0, &mut hit as *mut _ as isize)).ok()
        });
        if hover != self.drop_tab.get() {
            if let Some(old) = self.drop_tab.get() {
                send(self.tab_bar, TCM_HIGHLIGHTITEM, old, 0);
            }
            if let Some(new) = hover {
                send(self.tab_bar, TCM_HIGHLIGHTITEM, new, 1);
            }
            self.drop_tab.set(hover);
        }
        let (location, unavailable) = self.st.borrow().tabs.get(hover?).map(|t| (t.location.clone(), t.unavailable))?;
        // Not into This PC, not into a folder that failed to load, and not back where the files come from.
        let from_here = self.dragging_files.get() && location.same(&self.list.location());
        (matches!(location, Location::Dir(_)) && !unavailable && !from_here).then_some(location)
    }

    fn show_tabs(&self) {
        self.drop_tab.set(None); // the tabs are rebuilt, so no highlight is left
        let (names, active): (Vec<OsString>, usize) = {
            let st = self.st.borrow();
            (st.tabs.iter().map(|t| t.location.display_name()).collect(), st.active)
        };
        send(self.tab_bar, TCM_DELETEALLITEMS, 0, 0);
        for (i, name) in names.iter().enumerate() {
            let mut text: Vec<u16> = wide(name);
            if text.len() > 29 {
                text.truncate(27);
                text.extend_from_slice(&[0x2026, 0]);
            }
            let item = TCITEMW { mask: TCIF_TEXT, pszText: PWSTR(text.as_mut_ptr()), ..Default::default() };
            send(self.tab_bar, TCM_INSERTITEMW, i, &item as *const _ as isize);
        }
        send(self.tab_bar, TCM_SETCURSEL, active, 0);
    }

    // ------------------------------------------------------------ file commands

    fn delete_selection(&self, permanently: bool) {
        let rows = self.list.selection();
        let (Some(&first), Some(&last)) = (rows.first(), rows.last()) else { return };
        if matches!(self.list.location(), Location::ThisPc) {
            return;
        }
        // Afterwards, select the item that followed the deleted ones.
        let next = if last + 1 < self.list.count() { Some(last + 1) } else { first.checked_sub(1) };
        let mut view = next.and_then(|r| self.list.item(r)).map(|it| ViewState::selecting(it.name)).unwrap_or_default();
        view.top = Some(self.list.top_row());
        delete_items(self.wnd, &self.list.selected_items(), permanently);
        self.refresh(view);
    }

    fn new_folder(&self) {
        // The folder shown (none for This PC or an unavailable folder).
        let Location::Dir(dir) = self.list.location() else { return };
        let mut name = OsString::from("New folder");
        let mut i = 2;
        loop {
            match unsafe { CreateDirectoryW(pcwstr(&wide(dir.join(&name))), None) } {
                Ok(()) => break,
                Err(e) if e.code() == windows::Win32::Foundation::ERROR_ALREADY_EXISTS.to_hresult() && i <= 999 => {
                    name = format!("New folder ({i})").into();
                    i += 1;
                }
                Err(e) => {
                    self.set_message(e.message().trim());
                    return;
                }
            }
        }
        set_text(self.filter, "");
        let mut view = ViewState::selecting(name);
        view.rename = true;
        self.refresh(view);
    }

    // ------------------------------------------------------------ status and settings

    fn show_status(&self) {
        let rows = self.list.selection();
        let (mut bytes, mut files) = (0u64, 0usize);
        for it in rows.iter().filter_map(|&r| self.list.item(r)) {
            if !it.is_dir() {
                bytes += it.size;
                files += 1;
            }
        }
        let (count, total) = (self.list.count(), self.list.total());
        let mut s = format!("{} {}", count, if count == 1 { "item" } else { "items" });
        if count != total {
            s += &format!(" (of {total})");
        }
        if !rows.is_empty() {
            s += &format!("     {} selected", rows.len());
            if files > 0 {
                s += &format!("  {}", byte_size(bytes).to_string_lossy());
            }
        }
        let w = wide(s);
        send(self.status, SB_SETTEXTW, 0, w.as_ptr() as isize);
    }

    pub fn set_message(&self, text: &str) {
        let w = wide(text);
        send(self.status, SB_SETTEXTW, 1, w.as_ptr() as isize);
    }

    fn warn(&self) {
        if self.selftest.borrow().is_none() {
            unsafe {
                let _ = MessageBeep(MB_ICONWARNING); // self-tests trigger errors on purpose; keep them quiet
            }
        }
    }

    /// A number from qoqdir.ini; missing or not a number: the default.
    fn number(&self, key: &str, fallback: i32) -> i32 {
        let mut buf = [0u16; 32];
        let k = wide(key);
        unsafe { GetPrivateProfileStringW(w!("Window"), pcwstr(&k), w!(""), Some(&mut buf), pcwstr(&self.ini)) };
        from_wide(&buf).to_string_lossy().trim().parse().unwrap_or(fallback)
    }

    fn load_settings(&self, cmd: &CommandLine) {
        let dpi = self.dpi.get();
        self.tree_width.set(self.number("TreeWidth", 220));
        self.show_hidden.set(self.number("ShowHidden", 0) != 0);
        self.list.set_sort(SortKey::from_index(self.number("SortColumn", 0)), self.number("SortAscending", 1) != 0);
        let widths: Vec<i32> =
            [320, 150, 170, 90].iter().enumerate().map(|(i, &d)| scale(self.number(&format!("Column{i}"), d), dpi)).collect();
        self.list.set_column_widths(&widths);

        // Tabs are not remembered: the command line's folders, or the home folder.
        let mut tabs = cmd.folders.clone();
        if tabs.is_empty() {
            tabs.push(known_folder(&FOLDERID_Profile).map(Location::dir).unwrap_or(Location::ThisPc));
        }
        {
            let mut st = self.st.borrow_mut();
            for location in tabs {
                let tab = Self::make_tab(&mut st, location);
                st.tabs.push(tab);
            }
            st.active = 0;
        }

        let (w, h) = (self.number("Width", 0), self.number("Height", 0));
        unsafe {
            if w > 0 && h > 0 {
                let (x, y) = (self.number("X", 0), self.number("Y", 0));
                let wp = WINDOWPLACEMENT {
                    length: size_of::<WINDOWPLACEMENT>() as u32,
                    rcNormalPosition: RECT { left: x, top: y, right: x + w, bottom: y + h },
                    showCmd: if self.number("Maximized", 0) != 0 { SW_SHOWMAXIMIZED.0 as u32 } else { SW_SHOWNORMAL.0 as u32 },
                    ..Default::default()
                };
                let _ = SetWindowPlacement(self.wnd, &wp);
            } else {
                // A self-test never steals focus.
                let _ = ShowWindow(self.wnd, if self.use_settings { SW_SHOWNORMAL } else { SW_SHOWNOACTIVATE });
            }
        }
    }

    fn save_settings(&self) {
        if !self.use_settings {
            return;
        }
        unsafe {
            // Only numbers are saved: a plain ini file is fine.
            let number = |key: &str, value: i32| {
                let (k, v) = (wide(key), wide(value.to_string()));
                let _ = WritePrivateProfileStringW(w!("Window"), pcwstr(&k), pcwstr(&v), pcwstr(&self.ini));
            };

            let mut wp = WINDOWPLACEMENT { length: size_of::<WINDOWPLACEMENT>() as u32, ..Default::default() };
            let _ = GetWindowPlacement(self.wnd, &mut wp);
            let r = wp.rcNormalPosition;
            number("X", r.left);
            number("Y", r.top);
            number("Width", r.right - r.left);
            number("Height", r.bottom - r.top);
            // Also true when closed while minimized from a maximized window.
            let maximized = wp.showCmd == SW_SHOWMAXIMIZED.0 as u32 || wp.flags.0 & WPF_RESTORETOMAXIMIZED.0 != 0;
            number("Maximized", maximized as i32);
            number("TreeWidth", self.tree_width.get());
            number("ShowHidden", self.show_hidden.get() as i32);
            let (key, asc) = self.list.sort();
            number("SortColumn", key as i32);
            number("SortAscending", asc as i32);
            let dpi = self.dpi.get() as i64;
            for (i, w) in self.list.column_widths().iter().enumerate() {
                number(&format!("Column{i}"), (*w as i64 * 96 / dpi) as i32);
            }
        }
    }
}
