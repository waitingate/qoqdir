//! Cubic: a light, fast, portable file manager for Windows.
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

use crate::folder::Location;

fn main() {
    // A bug stops the app (panic = "abort"); leave a note saying where, since there is no console.
    std::panic::set_hook(Box::new(|info| {
        use std::io::Write;
        let trace = std::backtrace::Backtrace::force_capture();
        let log = std::env::temp_dir().join("cubic-crash.log");
        if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(log) {
            let _ = writeln!(f, "{info}\n\n{trace}\n");
        }
    }));
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

        let arg = std::env::args_os().nth(1);
        let self_test = arg.as_ref().is_some_and(|a| a.eq_ignore_ascii_case("--selftest"));
        let start = if self_test {
            Some(selftest::prepare())
        } else {
            // Optional folder given on the command line.
            arg.map(|a| folder::resolve_argument(&a)).filter(|l| matches!(l, Location::Dir(_)))
        };

        let instance = GetModuleHandleW(None).unwrap_or_default().into();
        let code = match app::App::create(instance, start, !self_test) {
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
