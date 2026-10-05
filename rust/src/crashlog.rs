//! Spec 4.10: an internal bug stops the program and %TEMP%\qoqdir-crash.log says why. Hooks: panics (the panic
//! hook) and CPU faults (`on_crash`). A fault's line is built in a stack buffer and written with Win32 calls
//! (the heap may be what broke); addresses are exe offsets (ASLR).

use std::fmt;
use std::io::Write;
use std::sync::OnceLock;

use windows::Win32::Foundation::CloseHandle;
use windows::Win32::Storage::FileSystem::{
    CreateFileW, FILE_APPEND_DATA, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, OPEN_ALWAYS, WriteFile,
};
use windows::Win32::System::Diagnostics::Debug::{EXCEPTION_POINTERS, IMAGE_NT_HEADERS64, SetUnhandledExceptionFilter};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::System::SystemInformation::GetLocalTime;
use windows::Win32::System::SystemServices::IMAGE_DOS_HEADER;
use windows::Win32::System::Threading::{GetCurrentProcess, SetThreadStackGuarantee, TerminateProcess};

use crate::APP_NAME;
use crate::util::{pcwstr, wide};

const EXCEPTION_EXECUTE_HANDLER: i32 = 1;

static LOG_PATH: OnceLock<Vec<u16>> = OnceLock::new();
static EXE: OnceLock<(usize, usize)> = OnceLock::new(); // where the exe is loaded, and its size

/// Call once, first thing in main.
pub fn install() {
    let _ = LOG_PATH.set(wide(std::env::temp_dir().join(format!("{}-crash.log", APP_NAME.to_lowercase()))));
    unsafe {
        let base = GetModuleHandleW(None).map_or(0, |h| h.0 as usize);
        if base != 0 {
            let dos = &*(base as *const IMAGE_DOS_HEADER);
            let nt = &*((base + dos.e_lfanew as usize) as *const IMAGE_NT_HEADERS64);
            let _ = EXE.set((base, nt.OptionalHeader.SizeOfImage as usize));
        }
        let mut guard = 64 * 1024; // after a stack overflow the filter runs on what is left of the stack: keep room
        let _ = SetThreadStackGuarantee(&mut guard);
        SetUnhandledExceptionFilter(Some(on_crash));
    }
    std::panic::set_hook(Box::new(|info| {
        log(format_args!("{info}"));
        // Exit 3: panic = "abort" would __fastfail with a false "stack buffer overrun" report.
        unsafe {
            let _ = TerminateProcess(GetCurrentProcess(), 3);
        }
    }));
}

/// "exe+0x1234" for an address in the exe; the bare address for one in a system DLL or a shell extension.
struct Where(usize);

impl fmt::Display for Where {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match EXE.get() {
            Some(&(base, size)) if (base..base + size).contains(&self.0) => write!(f, "exe+0x{:x}", self.0 - base),
            _ => write!(f, "0x{:x} (outside the exe)", self.0),
        }
    }
}

unsafe extern "system" fn on_crash(e: *const EXCEPTION_POINTERS) -> i32 {
    let r = unsafe { &*(*e).ExceptionRecord };
    log(format_args!("Exception 0x{:08X}  [{}]", r.ExceptionCode.0 as u32, Where(r.ExceptionAddress as usize)));
    EXCEPTION_EXECUTE_HANDLER // ends the process without Windows' own error window
}

/// Appends "date time  what" to the log.
fn log(what: fmt::Arguments) {
    let Some(path) = LOG_PATH.get() else { return };
    let mut line = [0u8; 4096];
    let n = {
        let mut w = std::io::Cursor::new(&mut line[..]);
        let t = unsafe { GetLocalTime() };
        let version = env!("CARGO_PKG_VERSION");
        let _ = write!(w, "{:04}-{:02}-{:02} {:02}:{:02}:{:02}  v{version}  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        let _ = write!(w, "{what}"); // cut short at the end of the buffer
        let _ = w.write_all(b"\r\n");
        w.position() as usize
    };
    unsafe {
        let Ok(f) = CreateFileW(pcwstr(path), FILE_APPEND_DATA.0, FILE_SHARE_READ, None, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, None)
        else {
            return;
        };
        let _ = WriteFile(f, Some(&line[..n]), None, None);
        let _ = CloseHandle(f);
    }
}
