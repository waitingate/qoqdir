//! Folder contents and path rules, with no UI, so all of it is unit-tested (`cargo test`).

use std::collections::HashMap;
use std::ffi::{OsStr, OsString};
use std::os::windows::ffi::{OsStrExt, OsStringExt};
use std::path::{Component, Path, PathBuf, Prefix};
use std::sync::Arc;

use windows::Win32::Foundation::{
    ERROR_FILE_NOT_FOUND, ERROR_NO_MORE_FILES, ERROR_OPERATION_ABORTED, FILETIME, HANDLE, LPARAM, SYSTEMTIME,
};
use windows::Win32::Globalization::{
    CSTR_EQUAL, CompareStringOrdinal, DATE_SHORTDATE, GetDateFormatEx, GetTimeFormatEx, LCMAP_SORTKEY, LCMapStringEx,
    NORM_IGNORECASE, SORT_DIGITSASNUMBERS, TIME_NOSECONDS,
};
use windows::Win32::UI::WindowsAndMessaging::CharLowerBuffW;
use windows::Win32::Storage::FileSystem::{
    FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_HIDDEN, FILE_FLAGS_AND_ATTRIBUTES, FIND_FIRST_EX_LARGE_FETCH,
    FindClose, FindExInfoBasic, FindExSearchNameMatch, FindFirstFileExW, FindNextFileW, GetDiskFreeSpaceExW,
    GetFileAttributesExW, GetFileAttributesW, GetFileExInfoStandard, GetLogicalDrives, INVALID_FILE_ATTRIBUTES,
    WIN32_FILE_ATTRIBUTE_DATA, WIN32_FIND_DATAW,
};
use windows::Win32::Foundation::CloseHandle;
use windows::Win32::System::Diagnostics::ToolHelp::{
    CreateToolhelp32Snapshot, PROCESSENTRY32W, Process32FirstW, Process32NextW, TH32CS_SNAPPROCESS,
};
use windows::Win32::System::Environment::ExpandEnvironmentStringsW;
use windows::Win32::System::Ole::{DROPEFFECT, DROPEFFECT_MOVE, MK_ALT};
use windows::Win32::System::SystemInformation::GetSystemDirectoryW;
use windows::Win32::System::SystemServices::{MK_CONTROL, MK_LBUTTON, MK_RBUTTON, MK_SHIFT, MODIFIERKEYS_FLAGS};
use windows::Win32::System::Threading::GetCurrentProcessId;
use windows::Win32::System::Time::{FileTimeToSystemTime, SystemTimeToTzSpecificLocalTime};
use windows::Win32::UI::Shell::{
    SHGFI_DISPLAYNAME, SHGFI_SMALLICON, SHGFI_SYSICONINDEX, SHGFI_TYPENAME,
    StrFormatByteSizeW, StrFormatKBSizeW, StrStrIW,
};
use windows::core::PCWSTR;

use crate::util::{from_wide, pcwstr, wide};

// ---------------------------------------------------------------- paths

/// Where a tab is. "This PC" (the drive list) is its own case, not a magic path.
#[derive(Clone, Debug, Default)]
pub enum Location {
    #[default]
    ThisPc,
    Dir(PathBuf),
}

/// "\\server" alone, with no share.
fn bare_server(p: &Path) -> bool {
    let w: Vec<u16> = p.as_os_str().encode_wide().collect();
    let sep = u16::from(b'\\');
    w.len() > 2 && w[0] == sep && w[1] == sep && !w[2..].contains(&sep)
}

/// A name from the root: one separator first ("\Windows"), not two (a network path).
fn from_root(name: &OsStr) -> bool {
    let is_sep = |c: u16| c == u16::from(b'\\') || c == u16::from(b'/');
    let mut w = name.encode_wide();
    w.next().is_some_and(is_sep) && !w.next().is_some_and(is_sep)
}

impl Location {
    /// A folder location with trailing separators removed (except on "C:\").
    pub fn dir(path: impl Into<PathBuf>) -> Location {
        let path: PathBuf = path.into();
        let mut w: Vec<u16> = path.as_os_str().encode_wide().collect();
        while w.len() > 3 && w.last() == Some(&(b'\\' as u16)) {
            w.pop();
        }
        Location::Dir(PathBuf::from(OsString::from_wide(&w)))
    }

    pub fn path(&self) -> Option<&Path> {
        match self {
            Location::ThisPc => None,
            Location::Dir(p) => Some(p),
        }
    }

    pub fn parent(&self) -> Location {
        match self {
            // "\\server" alone (no share) is the top of a network path, as in the C++ version; std would say "\".
            Location::Dir(p) if bare_server(p) => Location::ThisPc,
            Location::Dir(p) => match p.parent() {
                Some(q) if !q.as_os_str().is_empty() => Location::dir(q),
                _ => Location::ThisPc,
            },
            Location::ThisPc => Location::ThisPc,
        }
    }

    /// Full path of an item in this location (for This PC, the item is a drive like "C:\"). A name from the root
    /// ("\Windows", as typed) goes on the folder's own drive or share, as Path::join puts it, and on a server alone
    /// too, which std doesn't take as a root: it would leave just "\Windows".
    pub fn join(&self, name: &OsStr) -> PathBuf {
        match self {
            Location::ThisPc => PathBuf::from(name),
            Location::Dir(p) if bare_server(p) && from_root(name) => {
                let mut joined = p.clone().into_os_string();
                joined.push(name);
                joined.into()
            }
            Location::Dir(p) => p.join(name),
        }
    }

    pub fn same(&self, other: &Location) -> bool {
        match (self, other) {
            (Location::ThisPc, Location::ThisPc) => true,
            (Location::Dir(a), Location::Dir(b)) => equal_no_case(a.as_os_str(), b.as_os_str()),
            _ => false,
        }
    }

    /// Text for the address bar.
    pub fn text(&self) -> OsString {
        match self {
            Location::ThisPc => "This PC".into(),
            Location::Dir(p) => p.clone().into_os_string(),
        }
    }

    /// Short name for tabs and the window title.
    pub fn display_name(&self) -> OsString {
        match self {
            Location::ThisPc => "This PC".into(),
            Location::Dir(p) => match (p.file_name(), p.components().next()) {
                (Some(name), _) => name.to_os_string(),
                // A share root is named after its share, as in the C++ version; a drive root keeps "C:\".
                (None, Some(Component::Prefix(prefix))) => match prefix.kind() {
                    Prefix::UNC(_, share) | Prefix::VerbatimUNC(_, share) => share.to_os_string(),
                    _ => p.clone().into_os_string(),
                },
                _ => p.clone().into_os_string(),
            },
        }
    }

    /// Name of the child of `self` that leads to `descendant`, if it is inside.
    pub fn child_toward(&self, descendant: &Location) -> Option<OsString> {
        let desc = descendant.path()?;
        match self {
            Location::ThisPc => match desc.components().next() {
                Some(Component::Prefix(p)) => match p.kind() {
                    Prefix::Disk(letter) | Prefix::VerbatimDisk(letter) => {
                        Some(format!("{}:\\", (letter as char).to_ascii_uppercase()).into())
                    }
                    _ => None,
                },
                _ => None,
            },
            Location::Dir(ancestor) => {
                let mut a = ancestor.components();
                let mut d = desc.components();
                loop {
                    match (a.next(), d.next()) {
                        (None, Some(Component::Normal(child))) => return Some(child.to_os_string()),
                        (Some(x), Some(y)) if equal_no_case(x.as_os_str(), y.as_os_str()) => continue,
                        _ => return None,
                    }
                }
            }
        }
    }
}

pub fn is_directory(path: &Path) -> bool {
    let a = unsafe { GetFileAttributesW(pcwstr(&wide(path))) };
    a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY.0) != 0
}

pub fn exists(path: &Path) -> bool {
    unsafe { GetFileAttributesW(pcwstr(&wide(path))) != INVALID_FILE_ATTRIBUTES }
}

/// System32, as Windows names it.
pub fn system_dir() -> Option<PathBuf> {
    let mut buf = [0u16; 260];
    let n = unsafe { GetSystemDirectoryW(Some(&mut buf)) } as usize;
    (n > 0 && n < buf.len()).then(|| PathBuf::from(OsString::from_wide(&buf[..n])))
}

/// "C:\..." or "\\server\...", not a command like "cmd" (the same rule as the C++ version: a drive letter with a
/// backslash, or two backslashes, so "\\server" alone counts too and opens with Windows' own error).
pub fn is_absolute_path(text: &OsStr) -> bool {
    let w: Vec<u16> = text.encode_wide().take(3).collect();
    let (colon, sep) = (u16::from(b':'), u16::from(b'\\'));
    (w.len() >= 3 && w[1] == colon && w[2] == sep) || (w.len() > 2 && w[0] == sep && w[1] == sep)
}

/// Typed text that means a place in the current folder (".", "..\x", "sub\deeper"), not a command.
pub fn is_relative_path(text: &OsStr) -> bool {
    let w: Vec<u16> = text.encode_wide().collect();
    if w.is_empty() || is_absolute_path(text) {
        return false;
    }
    if w[0] == b'.' as u16 {
        return true; // ".", ".\sub", "..\other", "..\"
    }
    // Only the first word counts ("notepad a\b.txt" stays a command); a colon marks a web address ("https:\\x").
    let end = w.iter().position(|&c| c == b' ' as u16 || c == b'\t' as u16).unwrap_or(w.len());
    w[..end].contains(&(b'\\' as u16)) && !w[..end].contains(&(b':' as u16))
}

/// Cleans up a path (typed, or from a shortcut). Leaves %VARS% alone: a folder may really be named "%TEMP%".
pub fn normalize(text: &OsStr) -> Location {
    let junk = |c: &u16| *c == b' ' as u16 || *c == b'\t' as u16 || *c == b'"' as u16;
    let mut w: Vec<u16> = text.encode_wide().collect();
    while w.last().is_some_and(junk) {
        w.pop();
    }
    let start = w.iter().position(|c| !junk(c)).unwrap_or(w.len());
    w.drain(..start);
    if w.is_empty() || equal_no_case(&OsString::from_wide(&w), OsStr::new("This PC")) {
        return Location::ThisPc;
    }
    for c in w.iter_mut() {
        if *c == b'/' as u16 {
            *c = b'\\' as u16;
        }
    }
    if w.len() == 2 && w[1] == b':' as u16 {
        w.push(b'\\' as u16);
    }
    if is_absolute_path(&OsString::from_wide(&w)) {
        // Resolves "." and "..". Most paths fit on the stack; a longer one gets the size Windows asks for (with its
        // terminating zero), not 64 KB for every path, which a drag of 10,000 files had to fill one by one.
        let src: Vec<u16> = w.iter().copied().chain(Some(0)).collect();
        let full = |buf: &mut [u16]| {
            let n = unsafe { windows::Win32::Storage::FileSystem::GetFullPathNameW(pcwstr(&src), Some(buf), None) };
            n as usize
        };
        let mut small = [0u16; 260];
        let n = full(&mut small);
        if n > 0 && n < small.len() {
            w = small[..n].to_vec();
        } else if n > 0 {
            let mut buf = vec![0u16; n];
            let m = full(&mut buf);
            if m > 0 && m < buf.len() {
                w = buf[..m].to_vec();
            }
        }
    }
    if w.len() >= 2 && w[1] == b':' as u16 && (b'a' as u16..=b'z' as u16).contains(&w[0]) {
        w[0] -= 32; // drive letter to upper case
    }
    Location::dir(OsString::from_wide(&w))
}

// ---------------------------------------------------------------- typed text and the command line

/// Expands %VARS%, only in what the user typed (5.3).
pub fn expand_vars(text: &OsStr) -> OsString {
    let mut buf = vec![0u16; 32768];
    let n = unsafe { ExpandEnvironmentStringsW(pcwstr(&wide(text)), Some(&mut buf)) } as usize;
    if n > 0 && n <= buf.len() { from_wide(&buf) } else { text.to_os_string() }
}

/// A folder given on the command line, relative to the current one ("qoqdir .").
fn resolve_argument(arg: &OsStr) -> Location {
    // Clean it first (quotes, spaces, "D:" -> "D:\", as in Explorer), then resolve against the working folder only
    // what is still relative, so a quoted "D:\" from cmd (it arrives as D:") is not taken as D:'s current folder.
    let location = normalize(arg);
    match location.path() {
        Some(p) if !is_absolute_path(p.as_os_str()) => {
            let abs = std::path::absolute(p);
            normalize(abs.as_deref().map_or(p.as_os_str(), |a| a.as_os_str()))
        }
        _ => location,
    }
}

/// The command line: qoqdir [switches] [folders...] (see 5.10 in docs/SPEC.md).
#[derive(Default, Debug)]
pub struct CommandLine {
    pub folders: Vec<Location>, // -t= / --tab= folders and plain folders, in order, each once
    pub help: bool,             // -h or -?
    pub self_test: bool,        // --selftest
    pub error: Option<String>,  // a mistake to show with the help
}

/// The program this one was started from, as its exe file name in lower case ("powershell.exe"); None if unknown.
pub fn parent_process_name() -> Option<OsString> {
    let mut processes = Vec::new(); // (id, parent id, exe name)
    unsafe {
        let snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0).ok()?;
        let mut e = PROCESSENTRY32W { dwSize: size_of::<PROCESSENTRY32W>() as u32, ..Default::default() };
        let mut more = Process32FirstW(snap, &mut e).is_ok();
        while more {
            processes.push((e.th32ProcessID, e.th32ParentProcessID, from_wide(&e.szExeFile)));
            more = Process32NextW(snap, &mut e).is_ok();
        }
        let _ = CloseHandle(snap);
    }
    let me = unsafe { GetCurrentProcessId() };
    let parent = processes.iter().find(|p| p.0 == me)?.1;
    processes.into_iter().find(|p| p.0 == parent).map(|p| p.2.to_ascii_lowercase())
}

pub fn parse_command_line(args: &[OsString]) -> CommandLine {
    let mut cmd = CommandLine::default();
    for arg in args {
        let text = arg.to_string_lossy();
        if !text.starts_with('-') {
            cmd.folders.push(resolve_argument(arg)); // a plain folder
        } else if text.starts_with("--tab=") || text.starts_with("-t=") {
            // All after "=" is the folder, even a name that starts with "-". "-t=" is short for "--tab=", but Windows
            // PowerShell takes "-t=" for a switch of its own: unquoted, it splits "-t=my.folder" at the dot and keeps
            // "-t=$HOME" as typed, so there the folder after "-t=" needs quotes (5.10). The folder is taken from the
            // original argument so any name survives.
            let at = if text.starts_with("-t=") { 3 } else { 6 };
            let folder: Vec<u16> = arg.encode_wide().skip(at).collect();
            cmd.folders.push(resolve_argument(&OsString::from_wide(&folder)));
        } else {
            match text.as_ref() {
                "--selftest" => cmd.self_test = true,
                "-h" | "-?" | "--help" => cmd.help = true,
                _ => cmd.error = Some(format!("Unknown option: {text}")),
            }
        }
        if cmd.error.is_some() {
            break;
        }
    }
    cmd.folders.retain(|l| matches!(l, Location::Dir(_)));
    let mut unique: Vec<Location> = Vec::new(); // each folder once
    for folder in cmd.folders.drain(..) {
        if !unique.iter().any(|u| u.same(&folder)) {
            unique.push(folder);
        }
    }
    cmd.folders = unique;
    cmd
}

/// `--tab=` and the folder, for a Qoqdir started on it (Open in new window, 5.8), quoted so that the command-line rules
/// give it back whole.
pub fn tab_argument(folder: &Path) -> OsString {
    // Backslashes before a quote escape it (CommandLineToArgvW and Rust's std read them alike), so the ones the folder
    // ends in are doubled: "D:\\" comes back as D:\, where "D:\" would end in a quote.
    let text: Vec<u16> = folder.as_os_str().encode_wide().collect();
    let slashes = text.iter().rev().take_while(|&&c| c == u16::from(b'\\')).count();
    let mut arg = OsString::from("--tab=\"");
    arg.push(folder.as_os_str());
    arg.push("\\".repeat(slashes));
    arg.push("\"");
    arg
}

/// A command line's words, as Windows splits them (CommandLineToArgvW). The app reads its own through std, which
/// splits them the same way; the tests check what a new window is given.
#[cfg(any(test, feature = "selftest"))]
pub fn command_line_words(line: &OsStr) -> Vec<OsString> {
    use windows::Win32::Foundation::{HLOCAL, LocalFree};
    use windows::Win32::UI::Shell::CommandLineToArgvW;
    let mut count = 0;
    unsafe {
        let words = CommandLineToArgvW(pcwstr(&wide(line)), &mut count);
        let list = (0..count.max(0) as usize).map(|i| OsString::from_wide((*words.add(i)).as_wide())).collect();
        let _ = LocalFree(Some(HLOCAL(words.cast())));
        list
    }
}

// ---------------------------------------------------------------- reading a folder

#[derive(Clone, Debug, Default)]
pub struct Item {
    pub name: OsString,              // file name, or "C:\" for a drive
    pub label: Option<OsString>,     // shown instead of name (drives)
    // The type name shown, from the shell: the loader fills it (fill_types), the list what is left. Shared by the
    // items of one type: a copy in each cost 1.4 MB for 10,000.
    pub type_text: Option<Arc<OsStr>>,
    pub size: u64,
    pub modified: u64, // FILETIME as one number
    pub attrs: u32,
    pub icon: i32, // system image list index; -1 until first shown
}

impl Item {
    pub fn is_dir(&self) -> bool {
        self.attrs & FILE_ATTRIBUTE_DIRECTORY.0 != 0
    }
    pub fn shown(&self) -> &OsStr {
        self.label.as_deref().unwrap_or(&self.name)
    }
}

#[derive(Default)]
pub struct Listing {
    pub location: Location,
    pub items: Vec<Item>,
    pub error: Option<windows::core::Error>,
    pub is_file: bool, // with an error: the path exists but is a file
    pub written: u64,  // the folder's own last write, as it was before the read (folder_written)
}

impl Listing {
    /// No items, and why.
    fn failed(location: &Location, error: windows::core::Error) -> Listing {
        Listing { location: location.clone(), error: Some(error), ..Default::default() }
    }
}

/// Reads a folder; blocking, so off the UI thread. Once `stop` says true it ends with ERROR_OPERATION_ABORTED.
pub fn list_folder(location: &Location, show_hidden: bool, stop: Option<&dyn Fn() -> bool>) -> Listing {
    let mut r = Listing { location: location.clone(), ..Default::default() };
    let Location::Dir(dir) = location else {
        list_drives(&mut r);
        return r;
    };
    r.written = folder_written(dir); // before the read: a change during it then shows as a newer time
    struct FindGuard(HANDLE);
    impl Drop for FindGuard {
        fn drop(&mut self) {
            unsafe {
                let _ = FindClose(self.0);
            }
        }
    }
    let mut fd = WIN32_FIND_DATAW::default();
    let pattern = wide(dir.join("*"));
    let handle = unsafe {
        FindFirstFileExW(
            pcwstr(&pattern),
            FindExInfoBasic,
            &mut fd as *mut _ as *mut _,
            FindExSearchNameMatch,
            None,
            FIND_FIRST_EX_LARGE_FETCH,
        )
    };
    let guard = match handle {
        Ok(h) => FindGuard(h),
        Err(e) => {
            // An empty drive root has no "." entry, so "not found" just means empty.
            if !(e.code() == ERROR_FILE_NOT_FOUND.to_hresult() && is_directory(dir)) {
                r.error = Some(e);
                let a = unsafe { GetFileAttributesW(pcwstr(&wide(dir))) };
                r.is_file = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY.0) == 0;
            }
            return r;
        }
    };
    let mut seen = 0usize;
    loop {
        seen += 1;
        if seen.is_multiple_of(256) && stop.is_some_and(|stop| stop()) {
            return Listing::failed(location, ERROR_OPERATION_ABORTED.to_hresult().into());
        }
        let name = from_wide(&fd.cFileName);
        let dots = name == "." || name == "..";
        let hidden = fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN.0 != 0;
        if !dots && (show_hidden || !hidden) {
            r.items.push(Item {
                name,
                size: (u64::from(fd.nFileSizeHigh) << 32) | u64::from(fd.nFileSizeLow),
                modified: (u64::from(fd.ftLastWriteTime.dwHighDateTime) << 32)
                    | u64::from(fd.ftLastWriteTime.dwLowDateTime),
                attrs: fd.dwFileAttributes,
                icon: -1,
                ..Default::default()
            });
        }
        if let Err(e) = unsafe { FindNextFileW(guard.0, &mut fd) } {
            // ERROR_NO_MORE_FILES, or why the read broke off (e.g. the network went)
            if e.code() != ERROR_NO_MORE_FILES.to_hresult() {
                return Listing::failed(location, e); // never half a folder as all of it
            }
            break;
        }
    }
    // Grown by doubling, the vector has room for up to twice its items, kept while the folder is shown: 0.7 MB for
    // 10,000. Moving them to a block their size takes well under a millisecond.
    r.items.shrink_to_fit();
    r
}

/// The folder's own last-write time (0 if unknown): it changes with its contents.
pub fn folder_written(path: &Path) -> u64 {
    let mut data = WIN32_FILE_ATTRIBUTE_DATA::default();
    let _ = unsafe { GetFileAttributesExW(pcwstr(&wide(path)), GetFileExInfoStandard, &mut data as *mut _ as *mut _) };
    (u64::from(data.ftLastWriteTime.dwHighDateTime) << 32) | u64::from(data.ftLastWriteTime.dwLowDateTime)
}

/// "C:\" and so on, for the drive letters in use.
pub fn drive_roots() -> Vec<String> {
    let mask = unsafe { GetLogicalDrives() };
    (0..26u8).filter(|i| mask & (1 << i) != 0).map(|i| format!("{}:\\", (b'A' + i) as char)).collect()
}

fn list_drives(r: &mut Listing) {
    for root in drive_roots() {
        let root_w = wide(&root);
        let mut it = Item {
            name: root.into(),
            attrs: FILE_ATTRIBUTE_DIRECTORY.0,
            icon: -1,
            ..Default::default()
        };
        let flags = SHGFI_DISPLAYNAME | SHGFI_TYPENAME | SHGFI_SYSICONINDEX | SHGFI_SMALLICON;
        let mut text = None;
        if let Some(sfi) = crate::shell::file_info(&root_w, FILE_FLAGS_AND_ATTRIBUTES(0), flags) {
            it.label = Some(from_wide(&sfi.szDisplayName));
            text = Some(from_wide(&sfi.szTypeName));
            it.icon = sfi.iIcon;
        }
        let (mut free, mut total) = (0u64, 0u64);
        if unsafe { GetDiskFreeSpaceExW(pcwstr(&root_w), Some(&mut free), Some(&mut total), None) }.is_ok() {
            let mut t = text.take().unwrap_or_default();
            t.push(format!("  \u{b7}  {} free of {}", byte_size(free).to_string_lossy(), byte_size(total).to_string_lossy()));
            text = Some(t);
        }
        it.type_text = text.map(Arc::from);
        r.items.push(it);
    }
}

// ---------------------------------------------------------------- sorting and filtering

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SortKey {
    Name = 0,
    Modified = 1,
    Type = 2,
    Size = 3,
}

impl SortKey {
    pub fn from_index(i: i32) -> SortKey {
        match i {
            1 => SortKey::Modified,
            2 => SortKey::Type,
            3 => SortKey::Size,
            _ => SortKey::Name,
        }
    }
}

/// A string's sort key: comparing two keys byte by byte orders them exactly as StrCmpLogicalW (Explorer) does,
/// checked on every pair of 1,500 tricky names, without asking Windows again for each of the ~130,000
/// comparisons a 10,000-file sort makes.
fn sort_key(text: &OsStr) -> Vec<u8> {
    let text: Vec<u16> = text.encode_wide().collect();
    let flags = LCMAP_SORTKEY | NORM_IGNORECASE.0 | SORT_DIGITSASNUMBERS.0;
    // With LCMAP_SORTKEY the sizes count bytes: a buffer of n u16s holds the n-byte key, at its start.
    let key = |buf: &[u16], n: usize| unsafe { std::slice::from_raw_parts(buf.as_ptr().cast::<u8>(), n) }.to_vec();
    // First into a buffer on the stack, which holds most names' keys: asking Windows for the size first costs as much
    // as the key itself.
    let mut small = [0u16; 512];
    let n = unsafe { LCMapStringEx(PCWSTR::null(), flags, &text, Some(&mut small), None, None, LPARAM(0)) };
    if n > 0 {
        return key(&small, n as usize);
    }
    let n = unsafe { LCMapStringEx(PCWSTR::null(), flags, &text, None, None, None, LPARAM(0)) }.max(0) as usize;
    let mut buf = vec![0u16; n];
    if n > 0 {
        unsafe { LCMapStringEx(PCWSTR::null(), flags, &text, Some(&mut buf), None, None, LPARAM(0)) };
    }
    key(&buf, n)
}

/// Folders always first; names compare the way Explorer does ("file2" < "file10").
pub fn sort_items(items: &mut Vec<Item>, key: SortKey, ascending: bool) {
    use std::cell::OnceCell;
    use std::cmp::Ordering;
    // Sorts the items' numbers, then moves each item once. Sort keys are made on first use and kept: a date or
    // size sort needs the names only for ties.
    let names: Vec<OnceCell<Vec<u8>>> = items.iter().map(|_| OnceCell::new()).collect();
    // The type names' keys, one per type (a folder holds few), and the one each item has.
    let mut type_keys: Vec<Vec<u8>> = Vec::new();
    let type_of: Vec<usize> = if key == SortKey::Type {
        let mut seen: HashMap<&OsStr, usize> = HashMap::new();
        items
            .iter()
            .map(|it| {
                let t = it.type_text.as_deref().unwrap_or_default();
                *seen.entry(t).or_insert_with(|| {
                    type_keys.push(sort_key(t));
                    type_keys.len() - 1
                })
            })
            .collect()
    } else {
        Vec::new()
    };
    let mut order: Vec<usize> = (0..items.len()).collect();
    order.sort_by(|&i, &j| {
        let (a, b) = (&items[i], &items[j]);
        if a.is_dir() != b.is_dir() {
            return if a.is_dir() { Ordering::Less } else { Ordering::Greater };
        }
        let mut c = match key {
            SortKey::Modified => a.modified.cmp(&b.modified),
            SortKey::Size => a.size.cmp(&b.size),
            // By the type name shown in the list when known, else by extension.
            SortKey::Type => match (&a.type_text, &b.type_text) {
                (Some(_), Some(_)) if type_of[i] == type_of[j] => Ordering::Equal, // the same type needs no comparing
                (Some(_), Some(_)) => type_keys[type_of[i]].cmp(&type_keys[type_of[j]]),
                _ => {
                    let r = unsafe { CompareStringOrdinal(ext(&wide(&a.name)), ext(&wide(&b.name)), true) };
                    (r.0 - CSTR_EQUAL.0).cmp(&0)
                }
            },
            SortKey::Name => Ordering::Equal,
        };
        if c == Ordering::Equal {
            c = names[i].get_or_init(|| sort_key(&a.name)).cmp(names[j].get_or_init(|| sort_key(&b.name)));
        }
        if ascending { c } else { c.reverse() }
    });
    let mut slots: Vec<Option<Item>> = items.drain(..).map(Some).collect();
    items.extend(order.into_iter().filter_map(|i| slots[i].take()));
}

/// Case-insensitive substring match, or a wildcard match when the filter has * or ?, in one pattern or several
/// separated by ';' ("*.jpg; *.png"). `filter` ends in a NUL.
pub fn matches_filter(item: &Item, filter: &[u16]) -> bool {
    if filter.len() <= 1 {
        return true;
    }
    if !filter.iter().any(|&c| c == b'*' as u16 || c == b'?' as u16) {
        return !unsafe { StrStrIW(pcwstr(&wide(item.shown())), pcwstr(filter)) }.is_null();
    }
    let name = to_lower(item.shown());
    let patterns = to_lower(&OsString::from_wide(&filter[..filter.len() - 1]));
    patterns.split(|&c| c == b';' as u16).any(|p| {
        let spaces = p.iter().take_while(|&&c| c == b' ' as u16).count(); // spaces after a ';' don't count
        wildcard_match(&name, &p[spaces..])
    })
}

/// `*` is any run of characters and `?` any one, both in lower case. On a mismatch only the last star takes one more
/// character, so a name takes at most (name x pattern) steps. Windows' PathMatchSpecW tries every way of splitting
/// the name between the stars (8.8 s for "*a*a*a*a*a*a*a*b" on a 40-letter name), and under a double-byte code page
/// it misreads Chinese letters.
fn wildcard_match(name: &[u16], pattern: &[u16]) -> bool {
    let (star, any) = (b'*' as u16, b'?' as u16);
    let (mut n, mut p, mut back) = (0, 0, None);
    while n < name.len() {
        if p < pattern.len() && (pattern[p] == any || pattern[p] == name[n]) {
            (n, p) = (n + 1, p + 1);
        } else if p < pattern.len() && pattern[p] == star {
            back = Some((p, n));
            p += 1;
        } else if let Some((s, mark)) = back {
            back = Some((s, mark + 1));
            (n, p) = (mark + 1, s + 1);
        } else {
            return false;
        }
    }
    pattern[p..].iter().all(|&c| c == star)
}

/// After deleting `rows` (ascending), the row of the item that takes the first one's place, else the one before.
pub fn row_after_deleting(rows: &[usize], count: usize) -> Option<usize> {
    let first = *rows.first()?;
    let mut next = first;
    for &row in rows {
        // past the block of selected rows that starts at the first one
        if row != next {
            break;
        }
        next += 1;
    }
    if next < count { Some(next) } else { first.checked_sub(1) }
}

// ---------------------------------------------------------------- text

pub fn equal_no_case(a: &OsStr, b: &OsStr) -> bool {
    // Without case, ordinal matching maps each UTF-16 unit to one: texts of other lengths never match.
    if a.encode_wide().count() != b.encode_wide().count() {
        return false;
    }
    let a: Vec<u16> = a.encode_wide().collect();
    let b: Vec<u16> = b.encode_wide().collect();
    unsafe { CompareStringOrdinal(&a, &b, true) == CSTR_EQUAL }
}

pub fn to_lower(s: &OsStr) -> Vec<u16> {
    let mut w: Vec<u16> = s.encode_wide().collect();
    if !w.is_empty() {
        unsafe { CharLowerBuffW(&mut w) };
    }
    w
}

/// The extension part of a NUL-terminated wide name (".txt"), without the NUL.
fn ext(name: &[u16]) -> &[u16] {
    let name = &name[..name.len().saturating_sub(1)];
    match name.iter().rposition(|&c| c == b'.' as u16) {
        Some(dot) => &name[dot..],
        None => &[],
    }
}

/// Lower-case extension with the dot (".txt"), empty if none.
pub fn extension_of(name: &OsStr) -> String {
    // Looked for in the name's own bytes, as a '.' is never part of a longer character there: no UTF-16 copy of the
    // whole name, which a folder's read made twice for each item. An ASCII one, as nearly all are, is lowered here.
    let bytes = name.as_encoded_bytes();
    let Some(dot) = bytes.iter().rposition(|&b| b == b'.') else { return String::new() };
    if let Ok(ext) = std::str::from_utf8(&bytes[dot..])
        && ext.is_ascii()
    {
        return ext.to_ascii_lowercase();
    }
    let ext = unsafe { OsStr::from_encoded_bytes_unchecked(&bytes[dot..]) }; // split just before an ASCII '.'
    let mut w: Vec<u16> = ext.encode_wide().collect();
    unsafe { CharLowerBuffW(&mut w) };
    String::from_utf16_lossy(&w)
}

/// A .lnk or .url file.
pub fn is_shortcut(name: &OsStr) -> bool {
    matches!(extension_of(name).as_str(), ".lnk" | ".url")
}

pub fn format_size(bytes: u64) -> OsString {
    let mut buf = [0u16; 64];
    unsafe { StrFormatKBSizeW(bytes as i64, &mut buf) };
    from_wide(&buf)
}

pub fn byte_size(bytes: u64) -> OsString {
    let mut buf = [0u16; 64];
    unsafe { StrFormatByteSizeW(bytes as i64, &mut buf) };
    from_wide(&buf)
}

pub fn format_time(ft: u64) -> OsString {
    if ft == 0 {
        return OsString::new();
    }
    let ft = FILETIME { dwLowDateTime: ft as u32, dwHighDateTime: (ft >> 32) as u32 };
    let (mut utc, mut local) = (SYSTEMTIME::default(), SYSTEMTIME::default());
    unsafe {
        if FileTimeToSystemTime(&ft, &mut utc).is_err() || SystemTimeToTzSpecificLocalTime(None, &utc, &mut local).is_err() {
            return OsString::new();
        }
        let (mut d, mut t) = ([0u16; 64], [0u16; 64]);
        GetDateFormatEx(PCWSTR::null(), DATE_SHORTDATE, Some(&local), PCWSTR::null(), Some(&mut d), PCWSTR::null());
        GetTimeFormatEx(PCWSTR::null(), TIME_NOSECONDS, Some(&local), PCWSTR::null(), Some(&mut t));
        let mut s = from_wide(&d);
        s.push("  ");
        s.push(from_wide(&t));
        s
    }
}

/// An ini file's section as GetPrivateProfileSectionW gives it, "key=value" each ending in a nul: keys in lower case,
/// as the file's are matched, and of a key there twice the first. Spaces round both, and quotes round a value, go,
/// as GetPrivateProfileStringW drops them.
pub fn parse_ini_section(text: &[u16]) -> HashMap<String, String> {
    let mut values = HashMap::new();
    for line in text.split(|&c| c == 0) {
        let line = String::from_utf16_lossy(line);
        let Some((key, value)) = line.split_once('=') else { continue };
        let value = value.trim_matches([' ', '\t']);
        let value = ['"', '\''].iter().find_map(|&q| value.strip_prefix(q)?.strip_suffix(q)).unwrap_or(value);
        values.entry(key.trim_matches([' ', '\t']).to_lowercase()).or_insert_with(|| value.to_string());
    }
    values
}

// ---------------------------------------------------------------- drops

/// The keys a folder's drop target is told (5.8): a plain drag that could move files goes on as a right-button drag, so
/// Windows asks with its menu (Copy here, Move here...) instead of moving them at once. Ctrl, Shift or Alt held says
/// what to do, and a drag that can only copy has nothing to ask.
pub fn drop_keys(keys: MODIFIERKEYS_FLAGS, allowed: DROPEFFECT) -> MODIFIERKEYS_FLAGS {
    let deliberate = MK_RBUTTON.0 | MK_SHIFT.0 | MK_CONTROL.0 | MK_ALT; // a key or button that says what to do
    let plain = (keys.0 & MK_LBUTTON.0) != 0 && (keys.0 & deliberate) == 0;
    if plain && (allowed.0 & DROPEFFECT_MOVE.0) != 0 {
        MODIFIERKEYS_FLAGS((keys.0 & !MK_LBUTTON.0) | MK_RBUTTON.0)
    } else {
        keys
    }
}

/// Why a drop into `dir` would leave the `dragged` files where they are (4.8).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DropBack {
    /// `dir` is the folder one of them is in.
    Source,
    /// `dir` is one of them, or a folder inside one: a folder can't go into itself.
    Itself,
}

/// None when a drop into `dir` puts the `dragged` files somewhere else.
pub fn drop_back(dir: &Location, dragged: &[Location]) -> Option<DropBack> {
    // This PC carries nothing a folder holds.
    dragged.iter().filter(|path| path.path().is_some()).find_map(|path| {
        if dir.same(&path.parent()) {
            Some(DropBack::Source)
        } else if dir.same(path) || path.child_toward(dir).is_some() {
            Some(DropBack::Itself)
        } else {
            None
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use windows::Win32::UI::Shell::StrCmpLogicalW;

    fn dir(p: &str) -> Location {
        Location::dir(p)
    }
    fn text(l: &Location) -> String {
        l.text().to_string_lossy().into_owned()
    }
    fn item(name: &str, size: u64, is_dir: bool) -> Item {
        Item { name: name.into(), size, attrs: if is_dir { FILE_ATTRIBUTE_DIRECTORY.0 } else { 0 }, ..Default::default() }
    }
    fn dated(name: &str, modified: u64) -> Item {
        Item { modified, ..item(name, 0, false) }
    }
    fn names(items: &[Item]) -> String {
        items.iter().map(|i| i.name.to_string_lossy().into_owned()).collect::<Vec<_>>().join(",")
    }

    #[test]
    fn parent() {
        assert_eq!(text(&dir("C:\\Users\\me").parent()), "C:\\Users");
        assert_eq!(text(&dir("C:\\Users").parent()), "C:\\");
        assert!(matches!(dir("C:\\").parent(), Location::ThisPc));
        assert!(matches!(Location::ThisPc.parent(), Location::ThisPc));
        assert_eq!(text(&dir("\\\\server\\share\\dir").parent()), "\\\\server\\share");
        assert!(matches!(dir("\\\\server\\share").parent(), Location::ThisPc));
        assert!(matches!(dir("\\\\server").parent(), Location::ThisPc)); // a server alone: as the C++ version
    }

    #[test]
    fn join_and_names() {
        assert_eq!(dir("C:\\").join(OsStr::new("a")), PathBuf::from("C:\\a"));
        assert_eq!(Location::ThisPc.join(OsStr::new("D:\\")), PathBuf::from("D:\\"));
        assert_eq!(dir("C:\\a\\b").display_name(), "b");
        assert_eq!(dir("C:\\").display_name(), "C:\\");
        assert_eq!(dir("\\\\server\\share").display_name(), "share");
        assert_eq!(Location::ThisPc.display_name(), "This PC");
        assert_eq!(text(&dir("C:\\a\\\\")), "C:\\a");
    }

    #[test]
    fn normalize_typed_text() {
        assert_eq!(text(&normalize(OsStr::new("  \"c:/Windows/\"  "))), "C:\\Windows");
        assert_eq!(text(&normalize(OsStr::new("d:"))), "D:\\");
        assert_eq!(text(&normalize(OsStr::new("C:\\a\\..\\b"))), "C:\\b");
        assert!(matches!(normalize(OsStr::new("this pc")), Location::ThisPc));
        assert!(matches!(normalize(OsStr::new("   ")), Location::ThisPc));
        let cwd = std::env::current_dir().unwrap();
        // A relative name ("qoqdir x\%TEMP%") goes through the current folder, and its variables aren't expanded.
        assert!(resolve_argument(OsStr::new("x\\%TEMP%")).same(&Location::dir(cwd.join("x\\%TEMP%"))));
        assert!(resolve_argument(OsStr::new(".")).same(&Location::dir(cwd))); // "qoqdir ." from a terminal
        assert_eq!(text(&resolve_argument(OsStr::new("C:\\Windows\\"))), "C:\\Windows");
        assert_eq!(text(&resolve_argument(OsStr::new("C:"))), "C:\\"); // a drive alone is its root, as in Explorer
        assert_eq!(text(&resolve_argument(OsStr::new("C:\""))), "C:\\"); // cmd's quoted "C:\" arrives as C:": the root
        assert_eq!(text(&normalize(OsStr::new("\\\\server\\share\\"))), "\\\\server\\share");
        let windir = std::env::var_os("SystemRoot").unwrap();
        assert!(normalize(&expand_vars(OsStr::new("%SystemRoot%"))).same(&Location::dir(windir)));
        // A folder may really be named "%TEMP%": only typed text has its variables expanded.
        assert_eq!(text(&normalize(OsStr::new("C:\\a\\%TEMP%\\.\\sub"))), "C:\\a\\%TEMP%\\sub");
        assert_eq!(text(&resolve_argument(OsStr::new("C:\\a\\%TEMP%"))), "C:\\a\\%TEMP%"); // "qoqdir ." in such a folder
    }

    #[test]
    fn relative_paths() {
        // Typed text that means a place in the current folder (after normalize), not a command
        let rel = |s: &str| is_relative_path(OsStr::new(s));
        let typed = |s: &str| is_relative_path(&normalize(OsStr::new(s)).text());
        assert!(rel(".") && rel(".\\sub") && rel("..\\") && rel("..\\other") && rel("sub\\deeper"));
        assert!(rel("a\\b.txt")); // a relative file: the loader opens it with Windows
        assert!(typed("./sub"));
        assert!(!rel("sub")); // a bare name: the list decides (has_folder)
        assert!(!rel("cmd") && !typed("cmd /c dir") && !rel("notepad a\\b.txt"));
        assert!(!typed("https://example.com") && !rel("shell:Downloads"));
        assert!(!rel("C:\\Windows") && !rel("\\\\server\\share") && !rel(""));
        // A server without a share is a (bad) network path, not a place here: as in the C++ version.
        let abs = |s: &str| is_absolute_path(OsStr::new(s));
        assert!(abs("\\\\server") && !rel("\\\\server") && abs("C:\\") && abs("\\\\server\\share\\x"));
        assert!(!abs("C:a") && !abs("\\a") && !abs("cmd") && !abs("") && !abs("sub\\deeper"));
    }

    #[test]
    fn names_outside_ascii_and_long_paths() {
        // Chinese, and an emoji (a surrogate pair in UTF-16)
        let photo = dir("C:\\\u{5831}\u{544A}\\\u{1F600}.JPG");
        assert_eq!(photo.display_name(), "\u{1F600}.JPG");
        assert_eq!(text(&photo.parent()), "C:\\\u{5831}\u{544A}");
        assert_eq!(dir("C:\\\u{5831}\u{544A}").join(OsStr::new("\u{1F600}.JPG")), PathBuf::from("C:\\\u{5831}\u{544A}\\\u{1F600}.JPG"));
        // A name from the root, as typed ("\Windows"): on the folder's drive or share, as joinPath in C++ (5.3).
        assert_eq!(dir("C:\\x\\y").join(OsStr::new("\\Windows")), PathBuf::from("C:\\Windows"));
        assert_eq!(dir("C:\\").join(OsStr::new("\\Windows")), PathBuf::from("C:\\Windows"));
        assert_eq!(dir("C:\\x").join(OsStr::new("\\")), PathBuf::from("C:\\"));
        assert_eq!(dir("\\\\server\\share\\dir").join(OsStr::new("\\x")), PathBuf::from("\\\\server\\share\\x"));
        assert_eq!(dir("\\\\server\\share").join(OsStr::new("\\x")), PathBuf::from("\\\\server\\share\\x"));
        assert_eq!(dir("\\\\server").join(OsStr::new("\\x")), PathBuf::from("\\\\server\\x")); // a server alone too
        assert_eq!(extension_of(OsStr::new("\u{1F600}.JPG")), ".jpg");
        assert_eq!(text(&normalize(OsStr::new("c:/\u{5831}\u{544A}/"))), "C:\\\u{5831}\u{544A}");
        let deep = format!("C:\\{}\\b", "a".repeat(300)); // 305 characters, far beyond MAX_PATH
        assert_eq!(text(&normalize(OsStr::new(&format!("{deep}\\")))), deep);
        assert_eq!(text(&dir(&deep).parent()), deep[..303]);
        assert_eq!(dir(&deep).display_name(), "b");
    }

    #[test]
    fn row_after_deleting_rows() {
        // The item that takes the first deleted one's place, else the one before it
        assert_eq!(row_after_deleting(&[3, 4, 5], 10), Some(6));
        assert_eq!(row_after_deleting(&[9], 10), Some(8));
        assert_eq!(row_after_deleting(&[0, 9], 10), Some(1)); // not contiguous: row 1 moves into row 0's place
        assert_eq!(row_after_deleting(&[2, 7], 10), Some(3));
        assert_eq!(row_after_deleting(&(0..10).collect::<Vec<_>>(), 10), None);
        assert_eq!(row_after_deleting(&[], 5), None);
    }

    #[test]
    fn child_toward() {
        let c = |a: &Location, d: &str| a.child_toward(&dir(d)).map(|s| s.to_string_lossy().into_owned());
        assert_eq!(c(&dir("C:\\Users"), "C:\\Users\\me\\Documents").as_deref(), Some("me"));
        assert_eq!(c(&dir("C:\\"), "C:\\Users").as_deref(), Some("Users"));
        assert_eq!(c(&dir("c:\\users"), "C:\\Users\\me").as_deref(), Some("me"));
        assert_eq!(c(&Location::ThisPc, "d:\\Photos").as_deref(), Some("D:\\"));
        assert_eq!(c(&dir("C:\\Users"), "C:\\Users"), None);
        assert_eq!(c(&dir("C:\\Use"), "C:\\Users\\me"), None);
        assert_eq!(c(&dir("C:\\a"), "D:\\a\\b"), None);
        assert_eq!(c(&dir("\\\\server\\share"), "\\\\server\\share\\x\\y").as_deref(), Some("x"));
    }

    #[test]
    fn same_and_extension() {
        assert!(dir("C:\\Users").same(&dir("c:\\USERS")));
        assert!(!dir("C:\\Users").same(&dir("C:\\User")));
        assert!(!dir("C:\\").same(&Location::ThisPc));
        assert_eq!(extension_of(OsStr::new("Photo.JPG")), ".jpg");
        assert!(extension_of(OsStr::new("README")).is_empty());
        assert!(is_shortcut(OsStr::new("C:\\Bookmarks\\Work.LNK")) && is_shortcut(OsStr::new("site.url")));
        assert!(!is_shortcut(OsStr::new("notes.txt")));
    }

    #[test]
    fn sorting() {
        let mut v = vec![
            item("file10.txt", 5, false),
            item("b", 0, true),
            item("file2.txt", 50, false),
            item("a", 0, true),
            item("notes.md", 1, false),
        ];
        sort_items(&mut v, SortKey::Name, true);
        assert_eq!(names(&v), "a,b,file2.txt,file10.txt,notes.md");
        sort_items(&mut v, SortKey::Name, false);
        assert_eq!(names(&v), "b,a,notes.md,file10.txt,file2.txt");
        sort_items(&mut v, SortKey::Size, false);
        assert_eq!(names(&v), "b,a,file2.txt,file10.txt,notes.md");
        sort_items(&mut v, SortKey::Size, true); // 7.1: each column in both directions, folders first either way
        assert_eq!(names(&v), "a,b,notes.md,file10.txt,file2.txt");
        sort_items(&mut v, SortKey::Type, true);
        assert_eq!(names(&v), "a,b,notes.md,file2.txt,file10.txt");
        sort_items(&mut v, SortKey::Type, false);
        assert_eq!(names(&v), "b,a,file10.txt,file2.txt,notes.md");
    }

    #[test]
    fn sorting_by_date() {
        // Both directions; equal dates fall back to the name
        let mut v = vec![dated("x.txt", 3), dated("y.txt", 1), dated("p.txt", 2), dated("q.txt", 2), item("a", 0, true)];
        sort_items(&mut v, SortKey::Modified, true);
        assert_eq!(names(&v), "a,y.txt,p.txt,q.txt,x.txt");
        sort_items(&mut v, SortKey::Modified, false);
        assert_eq!(names(&v), "a,x.txt,q.txt,p.txt,y.txt");
    }

    #[test]
    fn sorting_by_type_name() {
        let mut v = vec![
            item("b.txt", 0, false),
            item("a.zip", 0, false),
            item("c.docx", 0, false),
            item("a.txt", 0, false),
            item("c.txt", 0, false),
        ];
        let type_of = |name: &str| Some(Arc::from(OsStr::new(name)));
        v[0].type_text = type_of("Text Document");
        v[1].type_text = type_of("Compressed (zipped) Folder");
        v[2].type_text = type_of("Microsoft Word Document");
        v[3].type_text = type_of("Text Document"); // the same type: by name
        v[4].type_text = type_of("text document"); // the same but for letter case: by name too
        sort_items(&mut v, SortKey::Type, true);
        assert_eq!(names(&v), "a.zip,c.docx,a.txt,b.txt,c.txt");
        sort_items(&mut v, SortKey::Type, false);
        assert_eq!(names(&v), "c.txt,b.txt,a.txt,c.docx,a.zip");
    }

    #[test]
    fn sorting_by_a_blank_type_name() {
        // A type the shell names as blank is known too: it sorts first, not by its extension among the others.
        let mut v = vec![item("z.cfg", 0, false), item("y.bat", 0, false), item("x.zip", 0, false)];
        let type_of = |name: &str| Some(Arc::from(OsStr::new(name)));
        v[0].type_text = type_of("");
        v[1].type_text = type_of("Windows Batch File");
        v[2].type_text = type_of("Compressed (zipped) Folder");
        sort_items(&mut v, SortKey::Type, true);
        assert_eq!(names(&v), "z.cfg,x.zip,y.bat");
    }

    #[test]
    fn ini_section() {
        // qoqdir.ini's [Window] section, read in one go: keys in lower case, the first of one there twice, no spaces
        // or quotes round them, and a line that is no key left out.
        let text: Vec<u16> = "X=10\0 Width = 300 \0x=99\0Quoted=\"5\"\0junk\0".encode_utf16().collect();
        let v = parse_ini_section(&text);
        assert_eq!(v.len(), 3);
        assert_eq!(v["x"], "10");
        assert_eq!(v["width"], "300");
        assert_eq!(v["quoted"], "5");
    }

    #[test]
    fn sorting_like_explorer() {
        // sort_items orders names exactly as Windows' StrCmpLogicalW (Explorer) does, tricky names included.
        let mut want = vec![
            "file10", "file2", "File1", "file01", "file 1", "file_1", "file-1", "file1a", "a", "_a", "(a)", "!a", "a b",
            "New folder (10)", "New folder", "New folder (2)", "v1.10", "v1.2.9", "\u{5831}\u{544A}10.pdf",
            "\u{5831}\u{544A}2.pdf", "\u{FF11}\u{FF12}", "\u{E4}bc", "abc", "stra\u{DF}e", "strasse", "\u{1F600}.jpg",
            "12345678901234567891", "12345678901234567890", "IMG_0100.JPG", "img_0099.jpg",
        ];
        let mut v: Vec<Item> = want.iter().map(|n| item(n, 0, false)).collect();
        sort_items(&mut v, SortKey::Name, true);
        want.sort_by(|a, b| unsafe { StrCmpLogicalW(pcwstr(&wide(a)), pcwstr(&wide(b))) }.cmp(&0));
        assert_eq!(names(&v), want.join(","));
    }

    #[test]
    fn command_line() {
        let args = |list: &[&str]| list.iter().map(OsString::from).collect::<Vec<_>>();
        let cwd = std::env::current_dir().unwrap();
        let c = parse_command_line(&args(&[".", "--tab=C:\\A", "--tab=c:\\a", "-t=C:\\My Files\\"]));
        assert!(c.error.is_none() && !c.help && !c.self_test); // c:\a is C:\A again: kept once
        let texts: Vec<String> = c.folders.iter().map(text).collect();
        assert_eq!(texts.len(), 3);
        assert!(c.folders[0].same(&Location::dir(cwd)));
        assert_eq!(texts[1..], ["C:\\A", "C:\\My Files"]);
        assert!(parse_command_line(&[]).folders.is_empty());
        assert!(parse_command_line(&args(&["-h"])).help && parse_command_line(&args(&["-?"])).help);
        assert!(parse_command_line(&args(&["--selftest"])).self_test);
        assert_eq!(parse_command_line(&args(&["-x"])).error.as_deref(), Some("Unknown option: -x"));
        // A network folder comes out as itself, and a name that starts with "-" is a folder too.
        assert_eq!(parse_command_line(&args(&["--tab=\\\\nas\\share\\"])).folders.iter().map(text).collect::<Vec<_>>(), ["\\\\nas\\share"]);
        let dash = parse_command_line(&args(&["--tab=-h"]));
        assert!(dash.error.is_none() && !dash.help && dash.folders.len() == 1);
        assert!(dash.folders[0].same(&Location::dir(std::env::current_dir().unwrap().join("-h"))));
        assert!(parse_command_line(&args(&["--tab=C:\\A", "-q"])).error.is_some());
        assert!(parse_command_line(&args(&["-n"])).error.is_some()); // -n no longer exists
        assert!(parse_command_line(&args(&["-tC:\\A"])).error.is_some()); // nor -t without "="
        // A trailing backslash is the same folder (one tab), and -t= takes a name that starts with "-" as a folder too.
        assert_eq!(parse_command_line(&args(&["C:\\A", "-t=C:\\A\\"])).folders.len(), 1);
        let td = parse_command_line(&args(&["-t=-h"]));
        assert!(td.error.is_none() && td.folders.len() == 1);
        assert!(td.folders[0].same(&Location::dir(std::env::current_dir().unwrap().join("-h"))));
    }

    #[test]
    fn tab_argument_comes_back_whole() {
        // The folder a new window gets (5.8) comes back whole through Windows' command-line rules, a root's closing
        // backslash and a name that looks like an option or a variable included.
        let folders =
            ["C:\\My Files", "D:\\", "\\\\nas\\share", "C:\\-h", "C:\\x\\%TEMP%", "C:\\\u{5831}\u{544A}\\new folder"];
        for folder in folders {
            let mut line = OsString::from("qoqdir.exe ");
            line.push(tab_argument(Path::new(folder)));
            let words = command_line_words(&line);
            assert_eq!(words.len(), 2);
            let c = parse_command_line(&words[1..]);
            assert!(c.error.is_none());
            assert_eq!(c.folders.iter().map(text).collect::<Vec<_>>(), [folder]);
        }
        // The parser would mend "D:\" (a stray closing quote goes, and D: means D:\), but the quoting is right by
        // itself.
        assert_eq!(tab_argument(Path::new("D:\\")), "--tab=\"D:\\\\\"");
    }

    #[test]
    fn filtering() {
        let photo = item("Holiday Photo.jpg", 0, false);
        assert!(matches_filter(&photo, &wide("photo")));
        assert!(matches_filter(&photo, &wide("")));
        assert!(!matches_filter(&photo, &wide("video")));
        assert!(matches_filter(&item("report.PDF", 0, false), &wide("*.pdf")));
        assert!(!matches_filter(&item("report.pdf.txt", 0, false), &wide("*.pdf")));
        // Letters beyond English, and their case: Windows' own matcher missed these under a Chinese code page.
        assert!(matches_filter(&item("\u{8CC7}\u{6599}.txt", 0, false), &wide("\u{8CC7}*")));
        assert!(matches_filter(&item("\u{C4}BC.txt", 0, false), &wide("\u{E4}bc*")));
        assert!(matches_filter(&item("a.png", 0, false), &wide("*.jpg; *.png")));
        assert!(!matches_filter(&item("a.gif", 0, false), &wide("*.jpg;*.png")));
        assert!(matches_filter(&item("abc", 0, false), &wide("a?c")) && !matches_filter(&item("ac", 0, false), &wide("a?c")));
        // Windows' matcher took 8.8 s here, 4 to 9 times longer with each star; the filter runs it on every file.
        let start = std::time::Instant::now();
        assert!(!matches_filter(&item(&"a".repeat(40), 0, false), &wide("*a*a*a*a*a*a*a*b")));
        assert!(start.elapsed().as_millis() < 100);
    }

    #[test]
    fn listing() {
        let l = list_folder(&dir("C:\\Windows"), false, None);
        assert!(l.error.is_none());
        assert!(!l.items.is_empty());
        assert!(!l.items.iter().any(|i| i.name == "." || i.name == ".."));
        let missing = list_folder(&dir("C:\\definitely\\not\\here"), false, None);
        assert!(missing.error.is_some() && !missing.is_file);
        assert!(!list_folder(&Location::ThisPc, false, None).items.is_empty());
        // A path that is a file is reported as such (the app then opens the file instead).
        let as_file = list_folder(&Location::dir(std::env::current_exe().unwrap()), false, None);
        assert!(as_file.error.is_some() && as_file.is_file);
        // A read that a newer load replaced stops early, keeping nothing (System32 has hundreds of entries).
        let system = Location::dir(system_dir().unwrap());
        let stopped = list_folder(&system, false, Some(&|| true));
        assert!(stopped.error.is_some_and(|e| e.code() == ERROR_OPERATION_ABORTED.to_hresult()) && stopped.items.is_empty());
        assert!(list_folder(&system, false, Some(&|| false)).items.len() > 256);
    }
    /// Runs this test program again on one test, with `QOQDIR_CHILD` set and TEMP moved to `temp`; its exit code.
    fn run_self(test: &str, child: &str, temp: &Path) -> i32 {
        std::process::Command::new(std::env::current_exe().unwrap())
            .args(["--exact", test, "--nocapture"])
            .env("QOQDIR_CHILD", child)
            .env("TEMP", temp)
            .env("TMP", temp)
            .output()
            .map_or(-1, |o| o.status.code().unwrap_or(-1))
    }

    // The program this one was started from is named (main.rs sends PowerShell, and only PowerShell, an Enter).
    #[test]
    fn parent_process() {
        if std::env::var_os("QOQDIR_CHILD").is_some_and(|c| c == "parent") {
            let me = std::env::current_exe().unwrap().file_name().unwrap().to_ascii_lowercase();
            std::process::exit(if parent_process_name() == Some(me) { 0 } else { 5 });
        }
        assert_eq!(run_self("folder::tests::parent_process", "parent", &std::env::temp_dir()), 0);
    }

    // A panic stops the program with exit code 3, and the crash log says what and where (4.10).
    #[test]
    fn panic_is_logged() {
        if std::env::var_os("QOQDIR_CHILD").is_some_and(|c| c == "panic") {
            crate::crashlog::install();
            panic!("the test's own panic");
        }
        let temp = std::env::temp_dir().join("qoqdir-unit-tests");
        let _ = std::fs::create_dir(&temp);
        let log = crate::crashlog::log_path(&temp);
        let _ = std::fs::remove_file(&log);
        assert_eq!(run_self("folder::tests::panic_is_logged", "panic", &temp), 3);
        let text = std::fs::read_to_string(&log).unwrap_or_default();
        assert!(text.contains("the test's own panic") && text.contains("folder.rs"), "{text}");
        let _ = std::fs::remove_dir_all(&temp);
    }

    #[test]
    fn drop_keys_ask_only_for_a_plain_drag_that_could_move() {
        // It goes on as a right-button drag, so Windows asks (5.8); a key held, a drag that can only copy, a right-drag
        // and the drop itself (no button down) go on as they are.
        use windows::Win32::System::Ole::{DROPEFFECT_COPY, DROPEFFECT_LINK};
        let any = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        assert_eq!(drop_keys(MK_LBUTTON, any), MK_RBUTTON);
        assert_eq!(drop_keys(MK_LBUTTON, DROPEFFECT_MOVE), MK_RBUTTON);
        for held in [MK_CONTROL, MK_SHIFT, MODIFIERKEYS_FLAGS(MK_ALT)] {
            assert_eq!(drop_keys(MK_LBUTTON | held, any), MK_LBUTTON | held);
        }
        assert_eq!(drop_keys(MK_LBUTTON, DROPEFFECT_COPY | DROPEFFECT_LINK), MK_LBUTTON);
        assert_eq!(drop_keys(MK_RBUTTON, any), MK_RBUTTON);
        assert_eq!(drop_keys(MODIFIERKEYS_FLAGS(0), any), MODIFIERKEYS_FLAGS(0));
    }

    #[test]
    fn drop_back_where_the_files_are() {
        // A drop into the folder the files are in, into a folder they carry or into a folder inside that one leaves
        // them where they are (4.8); a folder whose name only starts the same is another folder.
        let d = Location::dir;
        let file = [d(r"C:\w\a.txt")];
        let both = [d(r"C:\w\a.txt"), d(r"C:\w\sub")];
        assert_eq!(drop_back(&d(r"C:\w"), &file), Some(DropBack::Source));
        assert_eq!(drop_back(&d(r"c:\W"), &file), Some(DropBack::Source));
        assert_eq!(drop_back(&d(r"C:\w\sub"), &both), Some(DropBack::Itself));
        assert_eq!(drop_back(&d(r"C:\w\SUB\in"), &both), Some(DropBack::Itself));
        assert_eq!(drop_back(&d(r"C:\w\sub2"), &both), None);
        assert_eq!(drop_back(&d(r"C:\other"), &both), None);
        assert_eq!(drop_back(&d(r"C:\w"), &[]), None);
        assert_eq!(drop_back(&d(r"C:\"), &[d(r"C:\a.txt")]), Some(DropBack::Source));
        assert_eq!(drop_back(&d(r"\\srv\share"), &[d(r"\\srv\share\a.txt")]), Some(DropBack::Source));
        assert_eq!(drop_back(&d(r"\\srv\share\d\e"), &[d(r"\\srv\share\d")]), Some(DropBack::Itself));
    }
}
