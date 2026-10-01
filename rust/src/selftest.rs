//! `qoqdir.exe --selftest`: drives the real app through a scripted session and
//! checks the results on disk. Keys are fed straight into `App::on_key`, so the
//! test never sends global keyboard input to other windows.

use std::cell::RefCell;
use std::ffi::{OsStr, OsString};
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::time::Instant;

use windows::Win32::Foundation::{HGLOBAL, HWND, LPARAM, POINT, POINTL, RECT, WPARAM};
use windows::Win32::Graphics::Gdi::ClientToScreen;
use windows::Win32::System::Ole::DROPEFFECT_COPY;
use windows::Win32::System::SystemServices::MK_LBUTTON;
use windows::Win32::UI::Controls::{LVIR_LABEL, LVM_GETITEMRECT, TCM_GETITEMRECT, TVM_GETITEMRECT};
use windows::Win32::System::DataExchange::{CloseClipboard, GetClipboardData, OpenClipboard};
use windows::Win32::System::Memory::{GlobalLock, GlobalUnlock};
use windows::Win32::Storage::FileSystem::FILE_FLAGS_AND_ATTRIBUTES;
use windows::Win32::UI::Controls::{
    EM_GETSEL, EM_REPLACESEL, HTREEITEM, LVIF_IMAGE, LVITEMW, LVM_GETITEMW, SB_GETTEXTLENGTHW, SB_GETTEXTW, TVE_EXPAND,
    TVGN_CHILD, TVGN_NEXT, TVGN_ROOT, TVIF_CHILDREN, TVITEMW, TVM_EXPAND, TVM_GETITEMW, TVM_GETNEXTITEM,
};
use windows::Win32::UI::Input::KeyboardAndMouse::{
    GetFocus, SetFocus, VIRTUAL_KEY, VK_BACK, VK_DELETE, VK_DOWN, VK_ESCAPE, VK_F2, VK_F5, VK_LEFT, VK_RETURN, VK_RIGHT,
};
use windows::Win32::UI::Shell::{
    DragQueryFileW, FOLDERID_Desktop, HDROP, SHFILEINFOW, SHGFI_SMALLICON, SHGFI_SYSICONINDEX, SHGetFileInfoW,
};
use windows::Win32::UI::WindowsAndMessaging::{
    KillTimer, PostMessageW, SetTimer, SetWindowTextW, WM_CHAR, WM_CLOSE, WM_KEYDOWN, WM_MBUTTONUP,
    WM_SYSKEYDOWN, WM_TIMER,
};

use crate::app::{App, Key, Nav, TIMER_LOADING, TIMER_REFRESH, TIMER_SELFTEST, ViewState};
use crate::folder::{Location, SortKey, exists, is_directory, list_folder};
use crate::shell::{copy_to_clipboard, data_object, item_menu, known_folder, type_name};
use windows::Win32::UI::Shell::StrCmpLogicalW;
use crate::util::{send, from_wide, pcwstr, wide, window_text};

type Act = Rc<dyn Fn(&App)>;
type Check = Rc<dyn Fn(&App) -> bool>;

struct Step {
    name: &'static str,
    act: Option<Act>,
    done: Check,
    timeout_ms: u128,
    while_loading: bool, // start without waiting for the current folder load to finish
}

pub struct SelfTest {
    steps: Vec<Step>,
    index: usize,
    started: Option<Instant>,
    busy: bool,
    dir: PathBuf,
    log: String,
    failures: usize,
}

thread_local! {
    static NOTE: RefCell<String> = const { RefCell::new(String::new()) };
}

fn note(text: String) {
    NOTE.with(|n| *n.borrow_mut() = text);
}

pub fn test_dir() -> PathBuf {
    std::env::temp_dir().join(format!("{}-rs-selftest", crate::APP_NAME.to_lowercase()))
}

/// Makes a fresh test folder and returns it.
pub fn prepare() -> Location {
    let root = test_dir();
    let _ = std::fs::remove_dir_all(&root);
    let _ = std::fs::create_dir_all(root.join("sub"));
    for name in ["file1.txt", "file2.txt", "file10.txt", "\u{5831}\u{544A}.pdf"] {
        let _ = std::fs::write(root.join(name), "x");
    }
    Location::dir(root)
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

fn make_shortcut(lnk: &Path, target: &OsStr) {
    use windows::Win32::System::Com::{CLSCTX_INPROC_SERVER, CoCreateInstance, IPersistFile};
    use windows::Win32::UI::Shell::{IShellLinkW, ShellLink};
    use windows::core::Interface;
    unsafe {
        let Ok(link) = CoCreateInstance::<_, IShellLinkW>(&ShellLink, None, CLSCTX_INPROC_SERVER) else { return };
        let _ = link.SetPath(pcwstr(&wide(target)));
        if let Ok(file) = link.cast::<IPersistFile>() {
            let _ = file.Save(pcwstr(&wide(lnk)), true);
        }
    }
}


impl App {
    fn test_key(&self, vk: VIRTUAL_KEY, ctrl: bool, shift: bool, alt: bool) {
        let focus = unsafe { GetFocus() };
        let message = if alt { WM_SYSKEYDOWN } else { WM_KEYDOWN };
        if self.on_key(Key { message, code: vk.0 as usize, focus, ctrl, shift, alt }) {
            return;
        }
        // Not ours: let the control handle it, as a real key press would.
        send(focus, WM_KEYDOWN, vk.0 as usize, 0);
        // A real Enter/Esc also sends a character, but only while the control is still there to take it.
        if unsafe { GetFocus() } == focus && (vk == VK_RETURN || vk == VK_ESCAPE) {
            send(focus, WM_CHAR, vk.0 as usize, 0);
        }
    }

    fn test_type(&self, text: &str) {
        for c in text.encode_utf16() {
            let focus = unsafe { GetFocus() };
            let key = Key { message: WM_CHAR, code: c as usize, focus, ctrl: false, shift: false, alt: false };
            if !self.on_key(key) {
                send(focus, WM_CHAR, c as usize, 0);
            }
        }
    }

    fn at(&self, dir: &Path) -> bool {
        !self.is_loading() && self.st.borrow().tabs[self.st.borrow().active].location.same(&Location::dir(dir))
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

    fn set_edit_text(h: HWND, text: &str) {
        let w = wide(text);
        unsafe {
            let _ = SetWindowTextW(h, pcwstr(&w));
        }
    }

    pub fn start_self_test(&self) {
        let dir = test_dir();
        let p = |name: &str| dir.join(name);
        let mut steps: Vec<Step> = Vec::new();
        let mut add = |name: &'static str, act: Option<Act>, done: Check, timeout_ms: u128| {
            steps.push(Step { name, act, done, timeout_ms, while_loading: false });
        };
        let d = dir.clone();

        add("opens the start folder with all 5 items", None, Rc::new(move |a| a.at(&d) && a.list.count() == 5), 5000);
        add("shows a Chinese file name", None, Rc::new(|a| a.listed("\u{5831}\u{544A}.pdf")), 5000);
        add(
            "typing in the list filters it",
            Some(Rc::new(|a| {
                unsafe {
                    let _ = SetFocus(Some(a.list.hwnd()));
                }
                a.test_type("file");
            })),
            Rc::new(|a| a.list.count() == 3 && unsafe { GetFocus() } == a.filter),
            5000,
        );
        add(
            "Esc clears the filter and returns to the list",
            Some(Rc::new(|a| a.test_key(VK_ESCAPE, false, false, false))),
            Rc::new(|a| a.list.count() == 5 && unsafe { GetFocus() } == a.list.hwnd()),
            5000,
        );
        add(
            "Ctrl+C puts the file on the clipboard",
            Some(Rc::new(|a| {
                a.list.select(&["file1.txt".into()], Some(OsStr::new("file1.txt")));
                a.test_key(VIRTUAL_KEY(b'C' as u16), true, false, false);
            })),
            Rc::new(|_| {
                let files = clipboard_files();
                files.len() == 1 && files[0].file_name() == Some(OsStr::new("file1.txt"))
            }),
            5000,
        );
        let sub = p("sub");
        add(
            "filter + Enter opens a folder",
            Some(Rc::new(|a| {
                a.test_type("sub");
                a.test_key(VK_RETURN, false, false, false);
            })),
            Rc::new(move |a| a.at(&sub)),
            5000,
        );
        let pasted = p("sub\\file1.txt");
        add(
            "Ctrl+V pastes with the Windows copy engine",
            Some(Rc::new(|a| a.test_key(VIRTUAL_KEY(b'V' as u16), true, false, false))),
            Rc::new(move |_| exists(&pasted)),
            10000,
        );
        add("pasted file appears in the list by itself", None, Rc::new(|a| !a.is_loading() && a.listed("file1.txt")), 5000);
        let d = dir.clone();
        add(
            "Backspace goes up and selects the folder you came from",
            Some(Rc::new(|a| a.test_key(VK_BACK, false, false, false))),
            Rc::new(move |a| {
                a.at(&d) && a.list.focused_name().as_deref() == Some(OsStr::new("sub")) && a.list.selected_names().len() == 1
            }),
            5000,
        );
        let sub = p("sub");
        add("Alt+Left goes back", Some(Rc::new(|a| a.test_key(VK_LEFT, false, false, true))), Rc::new(move |a| a.at(&sub)), 5000);
        let d = dir.clone();
        add("Alt+Right goes forward", Some(Rc::new(|a| a.test_key(VK_RIGHT, false, false, true))), Rc::new(move |a| a.at(&d)), 5000);
        let new_folder = p("New folder");
        add(
            "Ctrl+Shift+N creates a folder and starts renaming it",
            Some(Rc::new(|a| a.test_key(VIRTUAL_KEY(b'N' as u16), true, true, false))),
            Rc::new(move |a| !a.is_loading() && a.list.renaming() && is_directory(&new_folder)),
            5000,
        );
        let (made, new_folder) = (p("made"), p("New folder"));
        add(
            "Enter in the rename box renames the new folder",
            Some(Rc::new(|a| {
                if let Some(edit) = a.list.edit_control() {
                    Self::set_edit_text(edit, "made");
                }
                a.test_key(VK_RETURN, false, false, false);
            })),
            Rc::new(move |a| {
                !a.is_loading()
                    && is_directory(&made)
                    && !exists(&new_folder)
                    && a.list.focused_name().as_deref() == Some(OsStr::new("made"))
            }),
            5000,
        );
        add(
            "F2 selects the name without its extension",
            Some(Rc::new(|a| {
                unsafe {
                    let _ = SetFocus(Some(a.list.hwnd()));
                }
                a.test_type("file2");
                a.test_key(VK_DOWN, false, false, false);
                a.test_key(VK_F2, false, false, false);
            })),
            Rc::new(|a| {
                let Some(edit) = a.list.edit_control() else { return false };
                let (mut from, mut to) = (0u32, 0u32);
                send(edit, EM_GETSEL, &mut from as *mut _ as usize, &mut to as *mut _ as isize);
                window_text(edit) == "file2.txt" && from == 0 && to == 5
            }),
            5000,
        );
        let (renamed, old) = (p("renamed.txt"), p("file2.txt"));
        add(
            "typing a new name keeps the extension",
            Some(Rc::new(|a| {
                if let Some(edit) = a.list.edit_control() {
                    let text = wide("renamed");
                    send(edit, EM_REPLACESEL, 1, text.as_ptr() as isize);
                }
                a.test_key(VK_RETURN, false, false, false);
            })),
            Rc::new(move |a| !a.is_loading() && exists(&renamed) && !exists(&old)),
            5000,
        );
        let gone = p("file10.txt");
        add(
            "Delete moves the file to the Recycle Bin",
            Some(Rc::new(|a| {
                Self::set_edit_text(a.filter, "");
                unsafe {
                    let _ = SetFocus(Some(a.list.hwnd()));
                }
                a.test_type("file10");
                a.test_key(VK_DOWN, false, false, false);
                a.test_key(VK_DELETE, false, false, false);
            })),
            Rc::new(move |a| !a.is_loading() && !exists(&gone)),
            10000,
        );
        let external = p("external.txt");
        add(
            "files created by other programs appear by themselves",
            Some(Rc::new(move |a| {
                Self::set_edit_text(a.filter, "");
                let _ = std::fs::write(&external, "x");
            })),
            Rc::new(|a| !a.is_loading() && a.listed("external.txt")),
            5000,
        );
        let (gone_dir, gone_dir2) = (p("gone"), p("gone"));
        add(
            "opening a folder",
            Some(Rc::new(move |a| {
                let _ = std::fs::create_dir(&gone_dir);
                a.open(Location::dir(&gone_dir), Nav::New, ViewState::default());
            })),
            Rc::new(move |a| a.at(&gone_dir2)),
            5000,
        );
        let (gone_dir, gone_dir2) = (p("gone"), p("gone"));
        add(
            "...that another program deletes stays in its tab, shown as not available",
            Some(Rc::new(move |_| {
                let _ = std::fs::remove_dir(&gone_dir);
            })),
            Rc::new(move |a| a.at(&gone_dir2) && a.list.count() == 0 && a.status_message().contains("not available")),
            5000,
        );
        let (gone_dir, gone_dir2) = (p("gone"), p("gone"));
        add(
            "F5 tries again once it is back",
            Some(Rc::new(move |a| {
                let _ = std::fs::create_dir(&gone_dir);
                a.test_key(VK_F5, false, false, false);
            })),
            Rc::new(move |a| a.at(&gone_dir2) && a.status_message().is_empty()),
            5000,
        );
        let d = dir.clone();
        add(
            "(back up to the test folder)",
            Some(Rc::new(|a| {
                unsafe {
                    let _ = SetFocus(Some(a.list.hwnd()));
                }
                a.test_key(VK_BACK, false, false, false);
            })),
            Rc::new(move |a| a.at(&d)),
            5000,
        );
        add(
            "Ctrl+T opens a tab",
            Some(Rc::new(|a| a.test_key(VIRTUAL_KEY(b'T' as u16), true, false, false))),
            Rc::new(|a| !a.is_loading() && a.st.borrow().tabs.len() == 2),
            5000,
        );
        let d = dir.clone();
        add(
            "Ctrl+W closes it",
            Some(Rc::new(|a| a.test_key(VIRTUAL_KEY(b'W' as u16), true, false, false))),
            Rc::new(move |a| a.st.borrow().tabs.len() == 1 && a.at(&d)),
            5000,
        );
        let d = dir.clone();
        add(
            "Enter on a missing typed path shows an error and stays put",
            Some(Rc::new(|a| {
                Self::set_edit_text(a.address, "C:\\no\\such\\folder\\anywhere");
                unsafe {
                    let _ = SetFocus(Some(a.address));
                }
                a.test_key(VK_RETURN, false, false, false);
            })),
            Rc::new(move |a| a.at(&d)),
            5000,
        );

        let (sub, sub2) = (p("sub"), p("sub"));
        add(
            "typing a folder path opens it",
            Some(Rc::new(move |a| {
                Self::set_edit_text(a.address, &sub.to_string_lossy());
                unsafe {
                    let _ = SetFocus(Some(a.address));
                }
                a.test_key(VK_RETURN, false, false, false);
            })),
            Rc::new(move |a| a.at(&sub2)),
            5000,
        );
        let started = Rc::new(RefCell::new(0u128));
        let (started1, started2) = (started.clone(), started.clone());
        add(
            "typing an unreachable network path does not freeze the window",
            Some(Rc::new(move |a| {
                Self::set_edit_text(a.address, "\\\\192.0.2.1\\share"); // reserved test address: nothing answers
                unsafe {
                    let _ = SetFocus(Some(a.address));
                }
                let t = Instant::now();
                a.test_key(VK_RETURN, false, false, false);
                *started1.borrow_mut() = t.elapsed().as_millis(); // how long Enter kept the window busy
                note(format!("Enter took {} ms", started1.borrow()));
            })),
            Rc::new(move |_| *started2.borrow() < 500),
            5000,
        );
        let (d, d2) = (dir.clone(), dir.clone());
        add(
            "...and opening another folder cancels it",
            Some(Rc::new(move |a| a.open(Location::dir(&d), Nav::New, ViewState::default()))),
            Rc::new(move |a| a.at(&d2)),
            5000,
        );

        // Second review: history entries for deleted folders, and shortcuts.
        let (t1, t1b) = (p("temp1"), p("temp1"));
        add(
            "(open a folder that will be deleted)",
            Some(Rc::new(move |a| {
                let _ = std::fs::create_dir(&t1);
                a.open(Location::dir(&t1), Nav::New, ViewState::default());
            })),
            Rc::new(move |a| a.at(&t1b)),
            5000,
        );
        let (d, d2) = (dir.clone(), dir.clone());
        add(
            "(go back to the test folder)",
            Some(Rc::new(move |a| a.open(Location::dir(&d), Nav::New, ViewState::default()))),
            Rc::new(move |a| a.at(&d2)),
            5000,
        );
        let (t1, t1b, d) = (p("temp1"), p("temp1"), dir.clone());
        add(
            "Back to a deleted folder drops it from history instead of getting stuck",
            Some(Rc::new(move |a| {
                let _ = std::fs::remove_dir(&t1);
                a.test_key(VK_LEFT, false, false, true);
            })),
            Rc::new(move |a| {
                let st = a.st.borrow();
                let last_back = st.tabs[st.active].back.last().cloned();
                drop(st);
                a.at(&d) && !last_back.is_some_and(|l| l.same(&Location::dir(&t1b)))
            }),
            5000,
        );
        let (lnk, target) = (p("sub-link.lnk"), p("sub"));
        add(
            "a shortcut to a folder opens it here",
            Some(Rc::new(move |a| {
                make_shortcut(&lnk, target.as_os_str());
                a.refresh(ViewState { select: vec!["sub-link.lnk".into()], focus: Some("sub-link.lnk".into()), ..Default::default() });
            })),
            Rc::new(|a| {
                if a.is_loading() || a.list.focused_name().as_deref() != Some(OsStr::new("sub-link.lnk")) {
                    return false;
                }
                unsafe {
                    let _ = SetFocus(Some(a.list.hwnd()));
                }
                a.test_key(VK_RETURN, false, false, false);
                true
            }),
            5000,
        );
        let sub = p("sub");
        add("(the shortcut's folder is shown)", None, Rc::new(move |a| a.at(&sub)), 5000);
        let (d, d2) = (dir.clone(), dir.clone());
        add(
            "(back up to the test folder)",
            Some(Rc::new(move |a| a.open(Location::dir(&d), Nav::New, ViewState::default()))),
            Rc::new(move |a| a.at(&d2)),
            5000,
        );
        let offline = p("offline.lnk");
        add(
            "(make a shortcut to an unreachable network folder)",
            Some(Rc::new(move |a| {
                make_shortcut(&offline, OsStr::new("\\\\192.0.2.1\\share"));
                a.refresh(ViewState { select: vec!["offline.lnk".into()], focus: Some("offline.lnk".into()), ..Default::default() });
            })),
            Rc::new(|a| !a.is_loading() && a.list.focused_name().as_deref() == Some(OsStr::new("offline.lnk"))),
            60000,
        );
        let started = Rc::new(RefCell::new(0u128));
        let (s1, s2) = (started.clone(), started.clone());
        add(
            "opening a shortcut to an unreachable network folder does not freeze the window",
            Some(Rc::new(move |a| {
                unsafe {
                    let _ = SetFocus(Some(a.list.hwnd()));
                }
                let t = Instant::now();
                a.test_key(VK_RETURN, false, false, false);
                *s1.borrow_mut() = t.elapsed().as_millis(); // how long Enter kept the window busy
                note(format!("Enter took {} ms", s1.borrow()));
            })),
            Rc::new(move |_| *s2.borrow() < 100),
            5000,
        );
        let (d, d2) = (dir.clone(), dir.clone());
        add(
            "(opening another folder cancels it)",
            Some(Rc::new(move |a| a.open(Location::dir(&d), Nav::New, ViewState::default()))),
            Rc::new(move |a| a.at(&d2)),
            5000,
        );

        // Regression tests for bugs found in code review.
        let (sub, sub2) = (p("sub"), p("sub"));
        add(
            "holding F5 while a folder loads does not start more loads",
            Some(Rc::new(move |a| {
                let before = a.generation.get();
                a.open(Location::dir(&sub), Nav::New, ViewState::default());
                unsafe {
                    let _ = SetFocus(Some(a.list.hwnd()));
                }
                for _ in 0..20 {
                    a.test_key(VK_F5, false, false, false); // like a held-down key
                }
                note(format!("{} load(s) started", a.generation.get() - before));
            })),
            Rc::new(move |a| a.at(&sub2) && NOTE.with(|n| n.borrow().as_str() == "1 load(s) started")),
            5000,
        );
        let (d, d2) = (dir.clone(), dir.clone());
        add(
            "(back up to the test folder again)",
            Some(Rc::new(move |a| a.open(Location::dir(&d), Nav::New, ViewState::default()))),
            Rc::new(move |a| a.at(&d2)),
            5000,
        );
        let (sub, sub2) = (p("sub"), p("sub"));
        add(
            "a background refresh never cancels a folder being opened",
            Some(Rc::new(move |a| {
                a.open(Location::dir(&sub), Nav::New, ViewState::default());
                send(a.wnd, WM_TIMER, TIMER_REFRESH, 0); // auto-refresh fires while "sub" is loading
            })),
            Rc::new(move |a| a.at(&sub2)),
            5000,
        );
        add(
            "\"Loading...\" goes away after a slow refresh",
            Some(Rc::new(|a| {
                a.refresh(ViewState::default());
                send(a.wnd, WM_TIMER, TIMER_LOADING, 0); // as if the refresh took over 250 ms
            })),
            Rc::new(|a| !a.is_loading() && a.status_message() != "Loading\u{2026}"),
            5000,
        );
        let d = dir.clone();
        add(
            "back up to the test folder",
            Some(Rc::new(|a| {
                unsafe {
                    let _ = SetFocus(Some(a.list.hwnd()));
                }
                a.test_key(VK_BACK, false, false, false);
            })),
            Rc::new(move |a| a.at(&d)),
            5000,
        );

        // Slow lookups happen on worker threads (so network drives cannot freeze the window).
        let (tool, tool2) = (p("tool.exe"), p("tool.exe"));
        add(
            "a program's own icon arrives from the background",
            Some(Rc::new(move |a| {
                let system = std::env::var_os("SystemRoot").map(PathBuf::from).unwrap_or_default().join("System32");
                let _ = std::fs::copy(system.join("cmd.exe"), &tool);
                a.refresh(ViewState::default());
            })),
            Rc::new(move |a| {
                let mut sfi = SHFILEINFOW::default();
                let path = wide(&tool2);
                unsafe {
                    SHGetFileInfoW(pcwstr(&path), FILE_FLAGS_AND_ATTRIBUTES(0), Some(&mut sfi), size_of::<SHFILEINFOW>() as u32, SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
                }
                let Some(row) = (0..a.list.count()).find(|&r| a.list.item(r).is_some_and(|i| i.name == "tool.exe")) else {
                    return false;
                };
                let mut item = LVITEMW { mask: LVIF_IMAGE, iItem: row as i32, ..Default::default() };
                send(a.list.hwnd(), LVM_GETITEMW, 0, &mut item as *mut _ as isize);
                !a.is_loading() && item.iImage == sfi.iIcon
            }),
            5000,
        );
        let tree = |a: &App| a.tree.hwnd();
        let (link2, sub, zip, html) = (p("sub-link2.lnk"), p("sub"), p("archive.zip"), p("page.html"));
        add(
            "sorting by Type puts the Type column in order",
            Some(Rc::new(move |a| {
                make_shortcut(&link2, sub.as_os_str()); // a few different types
                let _ = std::fs::write(&zip, "x");
                let _ = std::fs::write(&html, "x");
                a.refresh(ViewState::default());
            })),
            Rc::new(|a| {
                if a.is_loading() || a.list.total() < 8 {
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
            }),
            5000,
        );
        let file1 = p("file1.txt");
        add(
            "right-click menus can be built for This PC, a drive and a file",
            None,
            Rc::new(move |a| {
                item_menu(a.wnd, &[Location::ThisPc]).is_some()
                    && item_menu(a.wnd, &[Location::dir("C:\\")]).is_some()
                    && item_menu(a.wnd, &[Location::dir(&file1)]).is_some()
            }),
            5000,
        );
        let (made, file1, arrived) = (p("made"), p("file1.txt"), p("made\\file1.txt"));
        add(
            "dropping a file on a tab puts it in that tab's folder",
            Some(Rc::new(move |a| {
                a.new_tab(Location::dir(&made), false); // a second tab, right of the current one
                let index = a.st.borrow().active + 1;
                let mut r = RECT::default();
                send(a.tab_bar, TCM_GETITEMRECT, index, &mut r as *mut _ as isize);
                let mut pt = POINT { x: (r.left + r.right) / 2, y: (r.top + r.bottom) / 2 };
                let data = data_object(a.wnd, &[Location::dir(&file1)]); // what a drag carries
                unsafe {
                    let _ = ClientToScreen(a.tab_bar, &mut pt);
                    let (Some(data), Some(target)) = (data, a.tab_drop.borrow().clone()) else { return };
                    let cursor = POINTL { x: pt.x, y: pt.y };
                    let mut effect = DROPEFFECT_COPY;
                    let _ = target.DragEnter(&data, MK_LBUTTON, cursor, &mut effect);
                    effect = DROPEFFECT_COPY;
                    let _ = target.DragOver(MK_LBUTTON, cursor, &mut effect);
                    effect = DROPEFFECT_COPY;
                    let _ = target.Drop(&data, MK_LBUTTON, cursor, &mut effect);
                }
            })),
            Rc::new(move |a| exists(&arrived) && a.drop_tab.get().is_none()),
            10000,
        );
        add(
            "(close the extra tab)",
            Some(Rc::new(|a| {
                let index = a.st.borrow().active + 1;
                a.close_tab(index);
            })),
            Rc::new(|a| !a.is_loading() && a.st.borrow().tabs.len() == 1),
            5000,
        );
        let (sub, d) = (p("sub"), dir.clone());
        add(
            "middle-click on a folder opens it in a new background tab",
            Some(Rc::new(|a| {
                let row = (0..a.list.count()).find(|&r| a.list.item(r).is_some_and(|i| i.name == "sub"));
                if let Some(row) = row {
                    let mut r = RECT { left: LVIR_LABEL as i32, ..Default::default() };
                    send(a.list.hwnd(), LVM_GETITEMRECT, row, &mut r as *mut _ as isize);
                    let packed = ((r.left + r.right) / 2) as u16 as isize | ((((r.top + r.bottom) / 2) as u16 as isize) << 16);
                    send(a.list.hwnd(), WM_MBUTTONUP, 0, packed);
                }
            })),
            Rc::new(move |a| {
                let st = a.st.borrow();
                let opened = st.tabs.len() == 2 && st.tabs[st.active + 1].location.same(&Location::dir(&sub));
                drop(st);
                opened && a.at(&d)
            }),
            5000,
        );
        add(
            "(close that tab)",
            Some(Rc::new(|a| {
                let index = a.st.borrow().active + 1;
                a.close_tab(index);
            })),
            Rc::new(|a| !a.is_loading() && a.st.borrow().tabs.len() == 1),
            5000,
        );
        let d = dir.clone();
        add(
            "middle-click in the tree opens a new background tab",
            Some(Rc::new(|a| {
                let tree = a.tree.hwnd();
                let root = send(tree, TVM_GETNEXTITEM, TVGN_ROOT as usize, 0); // Desktop
                let mut r = RECT::default();
                unsafe { *(&mut r as *mut RECT as *mut isize) = root };   // TVM_GETITEMRECT takes the item in the rect
                send(tree, TVM_GETITEMRECT, 1, &mut r as *mut _ as isize);
                let packed = ((r.left + r.right) / 2) as u16 as isize | ((((r.top + r.bottom) / 2) as u16 as isize) << 16);
                send(tree, WM_MBUTTONUP, 0, packed);
            })),
            Rc::new(move |a| {
                let desktop = known_folder(&FOLDERID_Desktop).map(Location::dir).unwrap_or(Location::ThisPc);
                let st = a.st.borrow();
                let opened = st.tabs.len() == 2 && st.tabs[st.active + 1].location.same(&desktop);
                drop(st);
                opened && a.at(&d)
            }),
            5000,
        );
        add(
            "(close that tab too)",
            Some(Rc::new(|a| {
                let index = a.st.borrow().active + 1;
                a.close_tab(index);
            })),
            Rc::new(|a| !a.is_loading() && a.st.borrow().tabs.len() == 1),
            5000,
        );
        add(
            "the tree lists the drives under This PC",
            None,
            Rc::new(move |a| {
                let (mut node, mut last) = (send(tree(a), TVM_GETNEXTITEM, TVGN_ROOT as usize, 0), 0);
                while node != 0 {
                    last = node;
                    node = send(tree(a), TVM_GETNEXTITEM, TVGN_NEXT as usize, node);
                }
                last != 0 && send(tree(a), TVM_GETNEXTITEM, TVGN_CHILD as usize, last) != 0
            }),
            5000,
        );
        add(
            "expanding a tree folder lists its subfolders",
            Some(Rc::new(move |a| {
                let root = send(tree(a), TVM_GETNEXTITEM, TVGN_ROOT as usize, 0); // Desktop
                send(tree(a), TVM_EXPAND, TVE_EXPAND.0 as usize, root);
            })),
            Rc::new(move |a| {
                let desktop = known_folder(&FOLDERID_Desktop).map(Location::dir).unwrap_or(Location::ThisPc);
                let expected = list_folder(&desktop, false).items.iter().filter(|i| i.is_dir()).count();
                let root = send(tree(a), TVM_GETNEXTITEM, TVGN_ROOT as usize, 0);
                let mut shown = 0;
                let mut child = send(tree(a), TVM_GETNEXTITEM, TVGN_CHILD as usize, root);
                while child != 0 {
                    shown += 1;
                    child = send(tree(a), TVM_GETNEXTITEM, TVGN_NEXT as usize, child);
                }
                let mut item = TVITEMW { mask: TVIF_CHILDREN, hItem: HTREEITEM(root), ..Default::default() };
                send(tree(a), TVM_GETITEMW, 0, &mut item as *mut _ as isize);
                // Done when the subfolders are shown, or the node knows it has none.
                if expected > 0 { shown == expected } else { item.cChildren.0 == 0 }
            }),
            5000,
        );

        // Speed: a folder with 10,000 files.
        let big = p("big");
        let start = Rc::new(RefCell::new(Instant::now()));
        let (big1, start1) = (big.clone(), start.clone());
        let (big2, start2) = (big.clone(), start.clone());
        add(
            "loads a 10,000-file folder",
            Some(Rc::new(move |a| {
                let _ = std::fs::create_dir(&big1);
                for i in 0..10000 {
                    let _ = std::fs::write(big1.join(format!("item {i}.dat")), "x");
                }
                *start1.borrow_mut() = Instant::now();
                a.open(Location::dir(&big1), Nav::New, ViewState::default());
            })),
            Rc::new(move |a| {
                if !a.at(&big2) || a.list.count() != 10000 {
                    return false;
                }
                note(format!("load {} ms", start2.borrow().elapsed().as_millis()));
                true
            }),
            30000,
        );
        add(
            "filtering while scrolled to the bottom shows the matches from the top",
            Some(Rc::new(|a| {
                a.list.ensure_visible(Some(9999));
                Self::set_edit_text(a.filter, "99");
            })),
            Rc::new(|a| a.list.count() > 0 && a.list.top_row() == 0),
            5000,
        );
        add(
            "filters 10,000 files",
            Some(Rc::new(|a| {
                Self::set_edit_text(a.filter, "");
                let t = Instant::now();
                Self::set_edit_text(a.filter, "99");
                note(format!("filter {} ms", t.elapsed().as_millis()));
            })),
            Rc::new(|a| a.list.count() == (0..10000).filter(|i: &i32| i.to_string().contains("99")).count()),
            5000,
        );
        add(
            "sorts 10,000 files by date",
            Some(Rc::new(|a| {
                Self::set_edit_text(a.filter, "");
                let t = Instant::now();
                a.list.set_sort(SortKey::Modified, false);
                note(format!("sort {} ms", t.elapsed().as_millis()));
                a.list.set_sort(SortKey::Name, true);
            })),
            Rc::new(|a| a.list.count() == 10000),
            5000,
        );

        add(
            "copies 10,000 selected files to the clipboard",
            Some(Rc::new(|a| {
                a.list.select_all();
                let t = Instant::now();
                copy_to_clipboard(a.wnd, &a.list.selected_items(), false);
                note(format!("copy {} ms", t.elapsed().as_millis()));
            })),
            Rc::new(|_| clipboard_files().len() == 10000),
            5000,
        );

        // Steps that cancel an unreachable load must really cancel it (not wait ~40 s for it to
        // fail), or the app would then ask Windows to open the shortcut and a dialog would appear.
        for step in steps.iter_mut().filter(|s| s.name.contains("opening another folder cancels it")) {
            step.while_loading = true;
        }
        *self.selftest.borrow_mut() =
            Some(SelfTest { steps, index: 0, started: None, busy: false, dir, log: String::new(), failures: 0 });
        unsafe {
            SetTimer(Some(self.wnd), TIMER_SELFTEST, 50, None);
        }
    }

    pub fn self_test_tick(&self) {
        enum Next {
            Wait,
            Act(Option<Act>),
            Check(Check, u128),
            Finish,
        }
        // Decide under a short borrow; run the step's code with nothing borrowed,
        // because actions can run modal loops that tick again.
        let next = {
            let guard = self.selftest.borrow();
            let Some(t) = guard.as_ref() else { return };
            if t.busy {
                Next::Wait
            } else if t.index >= t.steps.len() {
                Next::Finish
            } else if t.started.is_none() {
                let step = &t.steps[t.index];
                if self.is_loading() && !step.while_loading { Next::Wait } else { Next::Act(step.act.clone()) }
            } else {
                let s = &t.steps[t.index];
                Next::Check(s.done.clone(), s.timeout_ms)
            }
        };
        match next {
            Next::Wait => {}
            Next::Act(act) => {
                note(String::new());
                self.with_test(|t| {
                    t.started = Some(Instant::now());
                    t.busy = true;
                });
                if let Some(act) = act {
                    act(self);
                }
                self.with_test(|t| t.busy = false);
            }
            Next::Check(done, timeout) => {
                let ok = done(self);
                let elapsed = self.with_test(|t| t.started.map_or(0, |s| s.elapsed().as_millis()));
                if !ok && elapsed < timeout {
                    return;
                }
                let note = NOTE.with(|n| n.borrow().clone());
                self.with_test(|t| {
                    let name = t.steps[t.index].name;
                    let slow = if elapsed > 500 { format!("   (slow step: {elapsed} ms)") } else { String::new() };
                    let note = if note.is_empty() { String::new() } else { format!("   [{note}]") };
                    t.log += &format!("{}{name}{note}{slow}\n", if ok { "ok    " } else { "FAIL  " });
                    t.failures += usize::from(!ok);
                    t.index += 1;
                    t.started = None;
                });
            }
            Next::Finish => {
                unsafe {
                    let _ = KillTimer(Some(self.wnd), TIMER_SELFTEST);
                }
                let (log, failures, dir) = self.with_test(|t| {
                    t.log += &if t.failures > 0 { format!("{} step(s) FAILED\n", t.failures) } else { "all steps passed\n".into() };
                    (t.log.clone(), t.failures, t.dir.clone())
                });
                let _ = std::fs::remove_dir_all(&dir);
                let mut log_path: OsString = dir.into_os_string();
                log_path.push(".log");
                let _ = std::fs::write(&log_path, log);
                self.exit_code.set(if failures > 0 { 1 } else { 0 });
                unsafe {
                    let _ = PostMessageW(Some(self.wnd), WM_CLOSE, WPARAM(0), LPARAM(0));
                }
            }
        }
    }

    fn with_test<R>(&self, f: impl FnOnce(&mut SelfTest) -> R) -> R {
        f(self.selftest.borrow_mut().as_mut().expect("self-test running"))
    }
}
