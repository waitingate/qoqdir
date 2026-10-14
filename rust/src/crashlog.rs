//! Spec 4.10: an internal bug stops the program and %TEMP%\qoqdir-crash.log says why. Hooks: panics (the panic
//! hook), CPU faults (`on_crash`) and a window that stops answering (`watch_for_hangs`). A fault's line is built in
//! a stack buffer and written with Win32 calls (the heap may be what broke); addresses are exe offsets (ASLR).

use std::fmt;
use std::io::Write;
use std::path::Path;
use std::sync::OnceLock;

use windows::Win32::Foundation::{CloseHandle, HMODULE, HWND};
use windows::Win32::Storage::FileSystem::{
    CreateFileW, FILE_APPEND_DATA, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, OPEN_ALWAYS, WriteFile,
};
use windows::Win32::System::Diagnostics::Debug::{
    CONTEXT, CONTEXT_CONTROL_AMD64, CONTEXT_INTEGER_AMD64, EXCEPTION_POINTERS, GetThreadContext,
    IMAGE_DIRECTORY_ENTRY_EXCEPTION, IMAGE_NT_HEADERS64, IMAGE_RUNTIME_FUNCTION_ENTRY, RtlVirtualUnwind,
    SetUnhandledExceptionFilter, UNW_FLAG_NHANDLER,
};
use windows::Win32::System::LibraryLoader::{
    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, GetModuleFileNameW,
    GetModuleHandleExW, GetModuleHandleW,
};
use windows::Win32::System::Memory::{MEM_IMAGE, MEMORY_BASIC_INFORMATION, VirtualQuery};
use windows::Win32::System::SystemInformation::GetLocalTime;
use windows::Win32::System::SystemServices::IMAGE_DOS_HEADER;
use windows::Win32::System::Threading::{
    GetCurrentProcess, GetCurrentThreadId, OpenThread, ResumeThread, SetThreadStackGuarantee, SuspendThread,
    THREAD_GET_CONTEXT, THREAD_SUSPEND_RESUME, TerminateProcess,
};
use windows::Win32::UI::WindowsAndMessaging::{IsHungAppWindow, IsWindow};
use windows::core::PCWSTR;

use crate::APP_NAME;
use crate::util::{from_wide, pcwstr, wide};

const EXCEPTION_EXECUTE_HANDLER: i32 = 1;

static LOG_PATH: OnceLock<Vec<u16>> = OnceLock::new();
static LOG_DIR: OnceLock<Vec<u16>> = OnceLock::new(); // set by log_in: the log there instead
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

/// The log goes into this folder from now on (a self-test keeps its own).
#[cfg_attr(not(feature = "selftest"), allow(dead_code))]
pub fn log_in(dir: &Path) {
    let _ = LOG_DIR.set(wide(dir.join(format!("{}-crash.log", APP_NAME.to_lowercase()))));
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

// ---------------------------------------------------------------- hangs

/// One frame as "exe+0x1234" or "name.dll+0x1234" (the bare address when it is in no module).
fn frame_text(pc: u64) -> String {
    let mut m = HMODULE::default();
    let flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if unsafe { GetModuleHandleExW(flags, PCWSTR(pc as usize as *const u16), &mut m) }.is_err() || m.is_invalid() {
        return format!("0x{pc:x}");
    }
    let base = m.0 as usize as u64;
    if EXE.get().is_some_and(|&(exe, _)| exe as u64 == base) {
        return format!("exe+0x{:x}", pc - base);
    }
    let mut buf = [0u16; 260];
    let n = unsafe { GetModuleFileNameW(Some(m), &mut buf) } as usize;
    let path = from_wide(&buf[..n]);
    let name = Path::new(&path).file_name().map_or(path.clone(), |n| n.to_os_string());
    format!("{}+0x{:x}", name.to_string_lossy(), pc - base)
}

#[repr(C, align(16))]
struct Aligned(CONTEXT); // GetThreadContext wants a 16-byte aligned record

/// The unwind entry for `pc`, from its module's own exception table, with the module's base. RtlLookupFunctionEntry
/// would do this, but it takes the loader's table lock: a thread stopped while the loader holds it (a DLL being
/// mapped) would never be resumed.
fn function_entry(pc: u64) -> Option<(u64, *const IMAGE_RUNTIME_FUNCTION_ENTRY)> {
    let mut mbi = MEMORY_BASIC_INFORMATION::default();
    let known = unsafe { VirtualQuery(Some(pc as usize as *const _), &mut mbi, size_of::<MEMORY_BASIC_INFORMATION>()) } != 0;
    if !known || mbi.Type != MEM_IMAGE {
        return None;
    }
    let image = mbi.AllocationBase as usize;
    let entries = unsafe {
        let dos = &*(image as *const IMAGE_DOS_HEADER);
        let nt = &*((image + dos.e_lfanew as usize) as *const IMAGE_NT_HEADERS64);
        let dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION.0 as usize];
        let table = (image + dir.VirtualAddress as usize) as *const IMAGE_RUNTIME_FUNCTION_ENTRY;
        std::slice::from_raw_parts(table, dir.Size as usize / size_of::<IMAGE_RUNTIME_FUNCTION_ENTRY>())
    };
    let rva = (pc - image as u64) as u32;
    let f = entries.get(entries.partition_point(|e| e.EndAddress <= rva)).filter(|e| e.BeginAddress <= rva)?;
    let unwind = unsafe { f.Anonymous.UnwindData };
    let f: *const IMAGE_RUNTIME_FUNCTION_ENTRY =
        if unwind & 1 != 0 { (image + (unwind & !1) as usize) as *const _ } else { f }; // an indirect entry
    Some((image as u64, f))
}

/// Once Windows calls the window not responding (5 s without a message read), logs where its thread is: each frame of
/// its stack as an exe offset or a DLL name, so a hung shell extension names itself. Call from the window's thread.
/// Every second, once per hang. While the thread is stopped nothing that takes a lock runs: its registers are read and
/// the frames walked from the modules' own unwind tables, into an array on the stack (a thread's stack in this process
/// is readable; a function without unwind data is a leaf, whose return address is on top of the stack). The frames are
/// named once it runs again: naming a module allocates and takes a loader lock, and a thread hung inside a DLL load (a
/// shell extension) holds both, so the watcher would wait for the very thread it has stopped, for good.
pub fn watch_for_hangs(window: HWND) {
    let (wnd, thread) = (window.0 as isize, unsafe { GetCurrentThreadId() });
    std::thread::spawn(move || {
        let wnd = HWND(wnd as *mut _);
        let mut said = false;
        loop {
            std::thread::sleep(std::time::Duration::from_secs(1));
            unsafe {
                if !IsWindow(Some(wnd)).as_bool() {
                    return;
                }
                if !IsHungAppWindow(wnd).as_bool() {
                    said = false;
                    continue;
                }
            }
            if said {
                continue;
            }
            said = true;
            let mut frames = [0u64; 24];
            let mut count = 0;
            unsafe {
                if let Ok(t) = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, false, thread) {
                    if SuspendThread(t) != u32::MAX {
                        let flags = CONTEXT_CONTROL_AMD64 | CONTEXT_INTEGER_AMD64;
                        let mut c = Aligned(CONTEXT { ContextFlags: flags, ..Default::default() });
                        if GetThreadContext(t, &mut c.0).is_ok() {
                            while count < frames.len() && c.0.Rip != 0 {
                                frames[count] = c.0.Rip;
                                count += 1;
                                let Some((base, f)) = function_entry(c.0.Rip) else {
                                    c.0.Rip = *(c.0.Rsp as *const u64);
                                    c.0.Rsp += 8;
                                    continue;
                                };
                                let (mut handler, mut frame) = (std::ptr::null_mut(), 0u64);
                                let _ = RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.0.Rip, f, &mut c.0, &mut handler, &mut frame, None);
                            }
                        }
                        ResumeThread(t);
                    }
                    let _ = CloseHandle(t);
                }
            }
            let mut line = String::from("Not responding for 5 s; the window thread is at");
            for (i, &pc) in frames[..count].iter().enumerate() {
                if line.len() > 1000 {
                    break;
                }
                line.push_str(if i == 0 { " " } else { " < " });
                line.push_str(&frame_text(pc));
            }
            log(format_args!("{line}"));
        }
    });
}

/// Appends "date time  what" to the log.
fn log(what: fmt::Arguments) {
    let Some(path) = LOG_DIR.get().or(LOG_PATH.get()) else { return };
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
