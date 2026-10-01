//! Qoqdir: a light, fast, portable file manager for Windows.
//!
//! qoqdir.exe [folder ...]   opens each folder as a tab (relative paths are fine: "qoqdir .")
//! qoqdir.exe --selftest     drives the app through a scripted session (see selftest.rs)
#![windows_subsystem = "windows"]

mod app;
mod filelist;
mod folder;
mod selftest;
mod shell;
mod tree;
mod util;

use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::System::Ole::{OleInitialize, OleUninitialize};
use windows::Win32::UI::Controls::{
    ICC_BAR_CLASSES, ICC_LISTVIEW_CLASSES, ICC_STANDARD_CLASSES, ICC_TAB_CLASSES, ICC_TREEVIEW_CLASSES,
    INITCOMMONCONTROLSEX, InitCommonControlsEx,
};
use windows::Win32::UI::WindowsAndMessaging::{DispatchMessageW, GetMessageW, MSG, TranslateMessage};

use std::ffi::OsString;

use windows::Win32::UI::WindowsAndMessaging::{MB_ICONINFORMATION, MB_ICONWARNING, MessageBoxW};

use crate::util::{pcwstr, wide};

const USAGE: &str = "Usage: qoqdir [switches] [folders...]\n\
\n\
  {folder}\tOpen the folder in a tab (relative paths work: qoqdir .)\n\
  -t{folder}\tThe same, as a switch: -tD:\\logs  -t\"C:\\My Files\"\n\
\t(quote folders with spaces, but don't end them with \\ before the closing quote)\n\
  -h, -?\tShow this help\n\
\n\
Tabs are not remembered between runs. Keep folders you use often as\n\
shortcuts in the Bookmarks folder next to qoqdir.exe (shown at the top of the tree).\n\
\n\
Examples:\n\
  qoqdir .\n\
  qoqdir -tD:\\projects -tD:\\logs";

/// The app's name: window title, settings file, logs. (The exe name is set in Cargo.toml.)
pub const APP_NAME: &str = "Qoqdir";

fn main() {
    // A bug stops the app (panic = "abort"); leave a note saying where, since there is no console.
    std::panic::set_hook(Box::new(|info| {
        use std::io::Write;
        let trace = std::backtrace::Backtrace::force_capture();
        let log = std::env::temp_dir().join(format!("{}-crash.log", APP_NAME.to_lowercase()));
        if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(log) {
            let _ = writeln!(f, "{info}\n\n{trace}\n");
        }
    }));
    let args: Vec<OsString> = std::env::args_os().skip(1).collect();
    let mut cmd = folder::parse_command_line(&args);
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

        if cmd.self_test {
            cmd.folders = vec![selftest::prepare()];
        }
        let self_test = cmd.self_test;
        let instance = GetModuleHandleW(None).unwrap_or_default().into();
        let code = match app::App::create(instance, cmd, !self_test) {
            Some(app) => {
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
                app.exit_code.get()
            }
            None => 1,
        };
        OleUninitialize();
        code
    };
    std::process::exit(code);
}
