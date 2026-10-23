//! Spec 4.10: an internal bug stops the program and %TEMP%\qoqdir-crash.log says why. Hooks: panics (the panic
//! hook), CPU faults (`on_crash`) and a window that stops answering (`watch_for_hangs`). A fault's line is built in
//! a stack buffer and written with Win32 calls (the heap may be what broke). Addresses are exe offsets (ASLR); to name
//! one, use the debug copy release.ps1 keeps: addr2line -e qoqdir-rust-<version>-debug.exe 0x140000000+offset

use std::fmt;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use windows::Win32::Foundation::{CloseHandle, HWND};
use windows::Win32::Storage::FileSystem::{
    CreateFileW, FILE_APPEND_DATA, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, OPEN_ALWAYS, WriteFile,
};
use windows::Win32::System::Diagnostics::Debug::{
    AddVectoredExceptionHandler, CONTEXT, CONTEXT_CONTROL_AMD64, CONTEXT_INTEGER_AMD64, EXCEPTION_POINTERS,
    GetThreadContext, IMAGE_DIRECTORY_ENTRY_EXCEPTION, IMAGE_NT_HEADERS64, IMAGE_RUNTIME_FUNCTION_ENTRY,
    RtlVirtualUnwind, SetUnhandledExceptionFilter, UNW_FLAG_NHANDLER,
};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::System::Memory::{
    MEM_COMMIT, MEM_IMAGE, MEMORY_BASIC_INFORMATION, PAGE_GUARD, PAGE_NOACCESS, VirtualQuery,
};
use windows::Win32::System::ProcessStatus::K32GetMappedFileNameW;
use windows::Win32::System::SystemInformation::GetLocalTime;
use windows::Win32::System::SystemServices::IMAGE_DOS_HEADER;
use windows::Win32::System::Threading::{
    GetCurrentProcess, GetCurrentThreadId, OpenThread, ResumeThread, SetThreadStackGuarantee, SuspendThread,
    THREAD_GET_CONTEXT, THREAD_SUSPEND_RESUME, TerminateProcess,
};
use windows::Win32::UI::WindowsAndMessaging::{IsHungAppWindow, IsWindow};

use crate::APP_NAME;
use crate::util::{from_wide, pcwstr, wide};

const EXCEPTION_EXECUTE_HANDLER: i32 = 1;
const EXCEPTION_CONTINUE_SEARCH: i32 = 0;
const EXCEPTION_ACCESS_VIOLATION: u32 = 0xC0000005;
const EXCEPTION_ILLEGAL_INSTRUCTION: u32 = 0xC000001D; // what the overflow trap raises, like the C++ build's

static LOG_PATH: OnceLock<Vec<u16>> = OnceLock::new();
static LOG_DIR: OnceLock<Vec<u16>> = OnceLock::new(); // set by log_in: the log there instead
static EXE: OnceLock<(usize, usize)> = OnceLock::new(); // where the exe is loaded, and its size

/// Call once, first thing in main.
pub fn install() {
    let _ = LOG_PATH.set(log_file(&std::env::temp_dir()));
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
        // A CPU fault on the main (window) thread is swallowed by the GNU runtime's start-up scope before the filter
        // above runs, so on_crash never sees it (a worker, outside that scope, still does). A vectored handler runs
        // first, on every thread, and catches a fatal fault in our own code there too (4.10).
        AddVectoredExceptionHandler(1, Some(on_vectored));
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
#[cfg(feature = "selftest")]
pub fn log_in(dir: &Path) {
    let _ = LOG_DIR.set(log_file(dir));
}

/// The crash log in a folder.
pub fn log_path(dir: &Path) -> PathBuf {
    dir.join(format!("{}-crash.log", APP_NAME.to_lowercase()))
}

fn log_file(dir: &Path) -> Vec<u16> {
    wide(log_path(dir))
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

// Runs before any frame's handler, on every thread. A fatal fault (an access violation or the overflow trap) in our
// own code is a bug of ours: log it and stop, which is the only way one on the main thread reaches the log. A fault
// elsewhere is left to run its course (the search goes on): an exception a shell extension raises and handles itself
// must not be turned into a crash (4.1), and a worker's unhandled one still reaches on_crash.
unsafe extern "system" fn on_vectored(e: *mut EXCEPTION_POINTERS) -> i32 {
    let r = unsafe { &*(*e).ExceptionRecord };
    let (code, addr) = (r.ExceptionCode.0 as u32, r.ExceptionAddress as usize);
    let in_exe = EXE.get().is_some_and(|&(base, size)| (base..base + size).contains(&addr));
    if in_exe && (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION) {
        log(format_args!("Exception 0x{code:08X}  [{}]", Where(addr)));
        unsafe {
            let _ = TerminateProcess(GetCurrentProcess(), code);
        }
    }
    EXCEPTION_CONTINUE_SEARCH
}

// ---------------------------------------------------------------- hangs

/// The image (the exe or a DLL) an address lies in: its base, or None outside any. From the memory manager, since
/// the loader's lock may be held by the very thread that is stopped (a shell extension stuck in a DLL load).
fn image_base(pc: u64) -> Option<usize> {
    let mut mbi = MEMORY_BASIC_INFORMATION::default();
    let known = unsafe { VirtualQuery(Some(pc as usize as *const _), &mut mbi, size_of::<MEMORY_BASIC_INFORMATION>()) } != 0;
    (known && mbi.Type == MEM_IMAGE).then_some(mbi.AllocationBase as usize)
}

/// One frame as "exe+0x1234" or "name.dll+0x1234" (the bare address when it is in no module). GetModuleHandleEx
/// would name the module but takes the loader lock, and then no line would ever be written.
fn frame_text(pc: u64) -> String {
    let Some(base) = image_base(pc) else {
        return format!("0x{pc:x}");
    };
    if EXE.get().is_some_and(|&(exe, _)| exe == base) {
        return format!("exe+0x{:x}", pc - base as u64);
    }
    let mut buf = [0u16; 260]; // "\Device\HarddiskVolume2\Windows\System32\shell32.dll"
    let n = unsafe { K32GetMappedFileNameW(GetCurrentProcess(), base as *const _, &mut buf) } as usize;
    let path = from_wide(&buf[..n]);
    let name = Path::new(&path).file_name().map_or(path.clone(), |n| n.to_os_string());
    format!("{}+0x{:x}", name.to_string_lossy(), pc - base as u64)
}

#[repr(C, align(16))]
struct Aligned(CONTEXT); // GetThreadContext wants a 16-byte aligned record

/// The unwind entry for `pc`, from its module's own exception table, with the module's base. RtlLookupFunctionEntry
/// would do this, but it takes the loader's table lock: a thread stopped while the loader holds it (a DLL being
/// mapped) would never be resumed.
fn function_entry(pc: u64) -> Option<(u64, *const IMAGE_RUNTIME_FUNCTION_ENTRY)> {
    let image = image_base(pc)?;
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

/// The return addresses on a thread's stack, innermost first, while it is stopped: its registers are read and the
/// frames walked from the modules' own unwind tables (nothing that takes a lock or allocates runs meanwhile). The one
/// rule that keeps the walk safe: every pointer it reads through lies in the committed stack around Rsp, which is
/// readable in this process. One outside it means a wrong unwind entry (an address that landed in a stranger's code),
/// and a fault here would end a program whose window was merely slow (4.10). The other two stops only spare the log a
/// tail of noise.
fn walk_stack(thread: u32) -> ([u64; 24], usize) {
    let mut frames = [0u64; 24];
    let mut count = 0;
    unsafe {
        let Ok(t) = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, false, thread) else {
            return (frames, 0);
        };
        if SuspendThread(t) != u32::MAX {
            let flags = CONTEXT_CONTROL_AMD64 | CONTEXT_INTEGER_AMD64;
            let mut c = Aligned(CONTEXT { ContextFlags: flags, ..Default::default() });
            let (mut lo, mut hi) = (0u64, 0u64);
            if GetThreadContext(t, &mut c.0).is_ok() {
                let mut stack = MEMORY_BASIC_INFORMATION::default();
                let known = VirtualQuery(Some(c.0.Rsp as usize as *const _), &mut stack, size_of::<MEMORY_BASIC_INFORMATION>()) != 0;
                if known && stack.State == MEM_COMMIT && !stack.Protect.contains(PAGE_GUARD) && !stack.Protect.contains(PAGE_NOACCESS) {
                    lo = stack.BaseAddress as usize as u64;
                    hi = lo + stack.RegionSize as u64;
                }
            }
            let on_stack = |p: u64| p >= lo && p + 8 <= hi;
            while count < frames.len() && c.0.Rip != 0 && on_stack(c.0.Rsp) {
                frames[count] = c.0.Rip;
                count += 1;
                let rsp = c.0.Rsp;
                let Some((base, f)) = function_entry(c.0.Rip) else {
                    if count > 1 {
                        break; // only the innermost frame can be a leaf: the walk is lost
                    }
                    c.0.Rip = *(c.0.Rsp as *const u64);
                    c.0.Rsp += 8;
                    continue;
                };
                // UNWIND_INFO's 4th byte: the frame register (0: none) and its offset; the unwinder reads through it.
                // Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8 to R15, in that order.
                let frame_reg = (*((base + (*f).Anonymous.UnwindData as u64) as *const u8).add(3) & 0xF) as usize;
                if frame_reg != 0 {
                    let registers = [c.0.Rax, c.0.Rcx, c.0.Rdx, c.0.Rbx, c.0.Rsp, c.0.Rbp, c.0.Rsi, c.0.Rdi, c.0.R8, c.0.R9, c.0.R10, c.0.R11, c.0.R12, c.0.R13, c.0.R14, c.0.R15];
                    if !on_stack(registers[frame_reg]) {
                        break;
                    }
                }
                let (mut handler, mut frame_base) = (std::ptr::null_mut(), 0u64);
                let _ = RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.0.Rip, f, &mut c.0, &mut handler, &mut frame_base, None);
                if c.0.Rsp <= rsp {
                    break; // not moving up: lost too
                }
            }
            ResumeThread(t);
        }
        let _ = CloseHandle(t);
    }
    (frames, count)
}

/// Once Windows calls the window not responding (5 s without a message read), logs where its thread is: each frame of
/// its stack as an exe offset or a DLL name, so a hung shell extension names itself. Call from the window's thread.
/// Every second, once per hang. The frames are named after the thread runs again, which allocates.
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
            let (frames, count) = walk_stack(thread);
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
