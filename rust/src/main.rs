//! Qoqdir: a light, fast, portable file manager for Windows (usage: COMMAND_LINE below; `--selftest`: selftest.rs,
//! only in a build with the `selftest` feature).
#![windows_subsystem = "windows"]

mod app;
mod crashlog;
mod filelist;
mod folder;
#[cfg(feature = "selftest")]
mod selftest;
mod shell;
mod tree;
mod util;

use std::ffi::OsString;

use windows::Win32::Foundation::{CloseHandle, GENERIC_READ, GENERIC_WRITE, HWND};
use windows::Win32::Storage::FileSystem::{
    CreateFileW, FILE_SHARE_READ, FILE_SHARE_WRITE, FILE_TYPE_DISK, FILE_TYPE_PIPE, FILE_TYPE_UNKNOWN, GetFileType,
    OPEN_EXISTING, WriteFile,
};
use windows::Win32::System::Console::{
    ATTACH_PARENT_PROCESS, AttachConsole, CONSOLE_SCREEN_BUFFER_INFO, FreeConsole, GetConsoleScreenBufferInfo,
    GetStdHandle, INPUT_RECORD, INPUT_RECORD_0, KEY_EVENT, KEY_EVENT_RECORD, KEY_EVENT_RECORD_0, STD_OUTPUT_HANDLE,
    WriteConsoleInputW, WriteConsoleW,
};
use windows::Win32::System::Diagnostics::Debug::{SEM_FAILCRITICALERRORS, SetErrorMode};
use windows::Win32::System::LibraryLoader::{GetModuleHandleW, LOAD_LIBRARY_SEARCH_SYSTEM32, SetDefaultDllDirectories};
use windows::Win32::System::Ole::{OleFlushClipboard, OleInitialize, OleUninitialize};
use windows::Win32::System::SystemInformation::GetSystemDirectoryW;
use windows::Win32::UI::Controls::{
    ICC_BAR_CLASSES, ICC_LISTVIEW_CLASSES, ICC_STANDARD_CLASSES, ICC_TAB_CLASSES, ICC_TREEVIEW_CLASSES,
    INITCOMMONCONTROLSEX, InitCommonControlsEx,
};
use windows::Win32::UI::Input::KeyboardAndMouse::VK_RETURN;
use windows::Win32::UI::WindowsAndMessaging::{
    DispatchMessageW, GetMessageW, MB_ICONINFORMATION, MB_ICONWARNING, MESSAGEBOX_STYLE, MSG, MessageBoxW,
    TranslateMessage,
};

use crate::util::{from_wide, pcwstr, wide};

// F1: the main keys in groups, the most helpful first in each: what Explorer users wouldn't guess comes first. One
// action a line, in plain words (the menus' own where there is one), with its main key: Explorer's other keys for the
// same thing work but aren't listed (Alt+Up, F4, Ctrl+R), nor are Enter and Ctrl+C. The command line last, by
// example, and where to read more. The line breaks are the text's own: a \ at a line end would also drop the next
// line's indent. No tab stops in either help: a message box puts them by the font's average letter width, so columns
// line up in one font only.
const KEYS: &str = "Navigation
  Filter: type in the list, or Ctrl+F
  Clear the filter: Esc
  Up: Backspace
  Bookmark a folder: Alt+drag it onto Bookmarks in the tree
  Address bar: Ctrl+L
  Back: Alt+Left
  Forward: Alt+Right
  Refresh: F5

Tabs
  New tab: Ctrl+T
  Close tab: Ctrl+W
  Save tabs: Ctrl+S
  Open saved tabs: Ctrl+O
  Open in new tab: Ctrl+Enter or middle-click
  Switch tab: Ctrl+Tab or Ctrl+1 to 9
  Move tab: Ctrl+Shift+PgUp or Ctrl+Shift+PgDn

Files
  Show or hide hidden files: Ctrl+H
  New folder: Ctrl+Shift+N
  Rename: F2
  Delete: Delete (to the Recycle Bin)
  Delete permanently: Shift+Delete
  Properties: Alt+Enter

Command line
  Open folders in tabs: qoqdir -t=\"D:\\logs\" -t=\"D:\\My Files\"
  Or: qoqdir --tab=\"D:\\logs\"
  More detail: type qoqdir --help in a terminal";

// qoqdir --help, -h, -?: printed in a terminal, so the columns line up.
const COMMAND_LINE: &str = concat!(
    "Qoqdir ",
    env!("CARGO_PKG_VERSION"),
    "

Usage: qoqdir [options] [folder path ...]

Opens each folder in a tab and shows the first one.
With no folder, opens your home folder.

Options:
  -t=\"folder path\", --tab=\"folder path\"   open the folder in a tab
  -h, --help, -?                          show this help

Examples:
  qoqdir .                               open the current folder
  qoqdir -t=\"D:\\logs\"                    open D:\\logs
  qoqdir -t=\"D:\\logs\" -t=\"D:\\My Files\"   open two tabs
  qoqdir \"D:\\logs\"                       same as -t=\"D:\\logs\"

Notes:
  Always quote folders. PowerShell needs the quotes after -t=.
  Don't end a quoted folder with a backslash: use \"D:\\My Files\",
  not \"D:\\My Files\\\". For a drive, use \"D:\" (not \"D:\\\").
  A folder that can't be opened yet (like a network drive that
  is not connected) still gets a tab.

Scripts:
  Each run opens a new window.
  A .bat file waits until the window is closed. PowerShell doesn't.
  Don't wait in a .bat file:  start \"\" qoqdir \"D:\\logs\"
  Wait in PowerShell:         Start-Process qoqdir \"D:\\logs\" -Wait
  Exit code: 0 = OK, 1 = error (such as an unknown option).

Files next to qoqdir.exe:
  qoqdir.ini       window size, sort order, column widths
  Bookmarks\\       shortcuts shown at the top of the tree
  Bookmarks\\Tabs\\  tabs saved with Ctrl+S
"
);

/// The app's name: window title, settings file, logs. (The exe name is set in Cargo.toml.)
pub const APP_NAME: &str = "Qoqdir";

/// A help box, titled with the version.
fn help_box(owner: Option<HWND>, text: &str, icon: MESSAGEBOX_STYLE) {
    let title = format!("{APP_NAME} {}", env!("CARGO_PKG_VERSION"));
    unsafe { MessageBoxW(owner, pcwstr(&wide(text)), pcwstr(&wide(title)), icon) };
}

/// Prints to a file or pipe (qoqdir -h > help.txt), else to the terminal it was typed in. False with neither (a
/// shortcut, Run): then the caller shows a box.
fn print_help(text: &str) -> bool {
    let text = text.replace('\n', "\r\n");
    unsafe {
        let out = GetStdHandle(STD_OUTPUT_HANDLE).unwrap_or_default();
        let kind = if out.is_invalid() { FILE_TYPE_UNKNOWN } else { GetFileType(out) };
        // PowerShell 7 closes the pipe at once for $h = qoqdir -h: then the terminal.
        if (kind == FILE_TYPE_DISK || kind == FILE_TYPE_PIPE) && WriteFile(out, Some(text.as_bytes()), None, None).is_ok() {
            return true;
        }
        if AttachConsole(ATTACH_PARENT_PROCESS).is_err() {
            return false;
        }
        let open = |name: &str| {
            let access = (GENERIC_READ | GENERIC_WRITE).0;
            let share = FILE_SHARE_READ | FILE_SHARE_WRITE;
            CreateFileW(pcwstr(&wide(name)), access, share, None, OPEN_EXISTING, Default::default(), None).unwrap_or_default()
        };
        let (con, input) = (open("CONOUT$"), open("CONIN$"));
        // cmd and PowerShell don't wait for a window program, so their prompt may be back already (cursor past
        // column 0). Print below it, then send Enter for a new prompt after the help. Without it PowerShell 7 draws
        // the next typed line over the help.
        let mut info = CONSOLE_SCREEN_BUFFER_INFO::default();
        let prompt = GetConsoleScreenBufferInfo(con, &mut info).is_ok() && info.dwCursorPosition.X > 0;
        let text: Vec<u16> = if prompt { "\r\n" } else { "" }.encode_utf16().chain(text.encode_utf16()).collect();
        let ok = WriteConsoleW(con, &text, None, None).is_ok();
        if ok && prompt {
            let key = |down: bool| INPUT_RECORD {
                EventType: KEY_EVENT as u16,
                Event: INPUT_RECORD_0 {
                    KeyEvent: KEY_EVENT_RECORD {
                        bKeyDown: down.into(),
                        wRepeatCount: 1,
                        wVirtualKeyCode: VK_RETURN.0,
                        wVirtualScanCode: 0,
                        uChar: KEY_EVENT_RECORD_0 { UnicodeChar: u16::from(b'\r') },
                        dwControlKeyState: 0,
                    },
                },
            };
            let mut n = 0;
            let _ = WriteConsoleInputW(input, &[key(true), key(false)], &mut n); // pressed and let go
        }
        let _ = CloseHandle(con);
        let _ = CloseHandle(input);
        let _ = FreeConsole();
        ok
    }
}

/// F1's help box: the main keys (qoqdir --help shows the command line).
pub fn show_keys(owner: HWND) {
    help_box(Some(owner), KEYS, MB_ICONINFORMATION);
}

fn main() {
    // DLLs that Windows loads by name come from System32 only, never from the exe's folder: a planted
    // propsys.dll next to qoqdir.exe (in Downloads, say) would run inside the app.
    unsafe {
        let _ = SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    crashlog::install();
    unsafe {
        SetErrorMode(SEM_FAILCRITICALERRORS); // a drive without a disk fails quietly, not with Windows' "No Disk" box
    }
    let args: Vec<OsString> = std::env::args_os().skip(1).collect();
    let mut cmd = folder::parse_command_line(&args); // "qoqdir ." is resolved here, against the terminal's folder
    // Then leave it: a program's working folder cannot be renamed, deleted or ejected. Explorer's is System32.
    let mut system = [0u16; 260];
    if unsafe { GetSystemDirectoryW(Some(&mut system)) } > 0 {
        let _ = std::env::set_current_dir(from_wide(&system));
    }
    #[cfg(not(feature = "selftest"))]
    if cmd.self_test {
        cmd.error = Some("Unknown option: --selftest".into()); // only a build with the selftest feature has it
    }
    if cmd.help || cmd.error.is_some() {
        let text = match &cmd.error {
            Some(error) => format!("{error}\n\n{COMMAND_LINE}"), // the mistake first
            None => COMMAND_LINE.to_string(),
        };
        if !print_help(&text) {
            help_box(None, &text, if cmd.error.is_some() { MB_ICONWARNING } else { MB_ICONINFORMATION });
        }
        std::process::exit(if cmd.error.is_some() { 1 } else { 0 });
    }
    let code = unsafe {
        if OleInitialize(None).is_err() {
            std::process::exit(1);
        }
        let mut shell_ref = shell::ShellProcessRef::new();
        let icc = INITCOMMONCONTROLSEX {
            dwSize: size_of::<INITCOMMONCONTROLSEX>() as u32,
            dwICC: ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES,
        };
        let _ = InitCommonControlsEx(&icc);

        #[cfg(feature = "selftest")]
        if cmd.self_test {
            cmd.folders = vec![selftest::prepare()];
        }
        let self_test = cmd.self_test;
        let instance = GetModuleHandleW(None).unwrap_or_default().into();
        let code = match app::App::create(instance, cmd, !self_test) {
            Some(app) => {
                #[cfg(feature = "selftest")]
                if self_test {
                    app.start_self_test();
                }
                let mut msg = MSG::default();
                while GetMessageW(&mut msg, None, 0, 0).0 > 0 {
                    if app.handle_key(&msg) {
                        continue;
                    }
                    let _ = TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                shell_ref.wait_for_shell(); // let background copies and Properties dialogs finish
                0
            }
            None => 1,
        };
        #[cfg(feature = "selftest")]
        let code = if self_test && code == 0 { selftest::exit_code() } else { code };
        let _ = OleFlushClipboard(); // files copied here stay pasteable after the app has closed (nothing to do otherwise)
        OleUninitialize();
        code
    };
    std::process::exit(code);
}
