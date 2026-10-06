//! Qoqdir: a light, fast, portable file manager for Windows (usage: USAGE below; `--selftest`: selftest.rs,
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

use windows::Win32::System::Diagnostics::Debug::{SEM_FAILCRITICALERRORS, SetErrorMode};
use windows::Win32::System::LibraryLoader::{GetModuleHandleW, LOAD_LIBRARY_SEARCH_SYSTEM32, SetDefaultDllDirectories};
use windows::Win32::System::Ole::{OleFlushClipboard, OleInitialize, OleUninitialize};
use windows::Win32::System::SystemInformation::GetSystemDirectoryW;
use windows::Win32::UI::Controls::{
    ICC_BAR_CLASSES, ICC_LISTVIEW_CLASSES, ICC_STANDARD_CLASSES, ICC_TAB_CLASSES, ICC_TREEVIEW_CLASSES,
    INITCOMMONCONTROLSEX, InitCommonControlsEx,
};
use windows::Win32::UI::WindowsAndMessaging::{
    DispatchMessageW, GetMessageW, MB_ICONINFORMATION, MB_ICONWARNING, MSG, MessageBoxW, TranslateMessage,
};

use crate::util::{from_wide, pcwstr, wide};

// The line breaks are the text's own: a \ at a line end would also drop the next line's indent.
const USAGE: &str = concat!("Version ", env!("CARGO_PKG_VERSION"), "

Usage: qoqdir [switches] [folders...]

  {folder}\tOpen the folder in a tab (relative paths work: qoqdir .)
  -t{folder}\tThe same, as a switch: -tD:\\logs  -t\"C:\\My Files\"
\t(quote folders with spaces, but don't end them with \\ before the closing quote)
  -h, -?\tShow this help

Keep folders you use often as shortcuts in the Bookmarks folder next to
qoqdir.exe (shown at the top of the tree). Tabs aren't remembered by themselves:
Ctrl+S saves them in Bookmarks\\Tabs, and Ctrl+O opens them again.

Examples:
  qoqdir .
  qoqdir -tD:\\projects -tD:\\logs");

/// The app's name: window title, settings file, logs. (The exe name is set in Cargo.toml.)
pub const APP_NAME: &str = "Qoqdir";

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
        cmd.error = Some("Unknown switch: --selftest".into()); // only a build with the selftest feature has it
    }
    if cmd.help || cmd.error.is_some() {
        // There is no console, so the help is a small window.
        let text = match &cmd.error {
            Some(error) => format!("{error}\n\n{USAGE}"),
            None => USAGE.to_string(),
        };
        let icon = if cmd.error.is_some() { MB_ICONWARNING } else { MB_ICONINFORMATION };
        unsafe { MessageBoxW(None, pcwstr(&wide(text)), pcwstr(&wide(APP_NAME)), icon) };
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
