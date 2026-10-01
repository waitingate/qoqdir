//! Folder contents and path rules. No UI here, so all of it is unit-tested
//! (`cargo test`).

use std::ffi::{OsStr, OsString};
use std::os::windows::ffi::{OsStrExt, OsStringExt};
use std::path::{Component, Path, PathBuf, Prefix};

use windows::Win32::Foundation::{ERROR_FILE_NOT_FOUND, FILETIME, HANDLE, SYSTEMTIME};
use windows::Win32::Globalization::{
    CSTR_EQUAL, CompareStringOrdinal, DATE_SHORTDATE, GetDateFormatEx, GetTimeFormatEx, TIME_NOSECONDS,
};
use windows::Win32::UI::WindowsAndMessaging::CharLowerBuffW;
use windows::Win32::Storage::FileSystem::{
    FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_HIDDEN, FILE_FLAGS_AND_ATTRIBUTES, FIND_FIRST_EX_LARGE_FETCH,
    FindClose, FindExInfoBasic, FindExSearchNameMatch, FindFirstFileExW, FindNextFileW, GetDiskFreeSpaceExW,
    GetFileAttributesW, GetLogicalDrives, INVALID_FILE_ATTRIBUTES, WIN32_FIND_DATAW,
};
use windows::Win32::System::Environment::ExpandEnvironmentStringsW;
use windows::Win32::System::Time::{FileTimeToSystemTime, SystemTimeToTzSpecificLocalTime};
use windows::Win32::UI::Shell::{
    PathMatchSpecW, SHFILEINFOW, SHGFI_DISPLAYNAME, SHGFI_SMALLICON, SHGFI_SYSICONINDEX, SHGFI_TYPENAME,
    SHGetFileInfoW, StrCmpLogicalW, StrFormatByteSizeW, StrFormatKBSizeW, StrStrIW,
};
use windows::core::PCWSTR;

use crate::util::{from_wide, pcwstr, wide};

/// Where a tab is. "This PC" (the drive list) is its own case, not a magic path.
#[derive(Clone, Debug)]
pub enum Location {
    ThisPc,
    Dir(PathBuf),
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
            Location::Dir(p) => match p.parent() {
                Some(q) if !q.as_os_str().is_empty() => Location::dir(q),
                _ => Location::ThisPc,
            },
            Location::ThisPc => Location::ThisPc,
        }
    }

    /// Full path of an item in this location (for This PC, the item is a drive like "C:\").
    pub fn join(&self, name: &OsStr) -> PathBuf {
        match self {
            Location::ThisPc => PathBuf::from(name),
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
            Location::Dir(p) => p.file_name().map(OsStr::to_os_string).unwrap_or_else(|| p.clone().into_os_string()),
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

pub fn equal_no_case(a: &OsStr, b: &OsStr) -> bool {
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

/// Cleans up what the user typed in the address bar.
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
    if w.contains(&(b'%' as u16)) {
        let src: Vec<u16> = w.iter().copied().chain(Some(0)).collect();
        let mut buf = vec![0u16; 32768];
        let n = unsafe { ExpandEnvironmentStringsW(pcwstr(&src), Some(&mut buf)) } as usize;
        if n > 0 && n <= buf.len() {
            w = from_wide(&buf).encode_wide().collect();
        }
    }
    for c in w.iter_mut() {
        if *c == b'/' as u16 {
            *c = b'\\' as u16;
        }
    }
    if w.len() == 2 && w[1] == b':' as u16 {
        w.push(b'\\' as u16);
    }
    let is_unc = w.len() > 2 && w[0] == b'\\' as u16 && w[1] == b'\\' as u16;
    let is_drive = w.len() >= 3 && w[1] == b':' as u16 && w[2] == b'\\' as u16;
    if is_unc || is_drive {
        let src: Vec<u16> = w.iter().copied().chain(Some(0)).collect();
        let mut buf = vec![0u16; 32768];
        let n = unsafe { windows::Win32::Storage::FileSystem::GetFullPathNameW(pcwstr(&src), Some(&mut buf), None) }
            as usize;
        if n > 0 && n < buf.len() {
            w = buf[..n].to_vec(); // resolves "." and ".."
        }
    }
    if w.len() >= 2 && w[1] == b':' as u16 && (b'a' as u16..=b'z' as u16).contains(&w[0]) {
        w[0] -= 32; // drive letter to upper case
    }
    Location::dir(OsString::from_wide(&w))
}

/// A folder given on the command line, relative to the current one ("cubic.exe .").
pub fn resolve_argument(arg: &OsStr) -> Location {
    normalize(std::path::absolute(arg).as_deref().map_or(arg, |p| p.as_os_str()))
}

pub fn is_directory(path: &Path) -> bool {
    let a = unsafe { GetFileAttributesW(pcwstr(&wide(path))) };
    a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY.0) != 0
}

pub fn exists(path: &Path) -> bool {
    unsafe { GetFileAttributesW(pcwstr(&wide(path))) != INVALID_FILE_ATTRIBUTES }
}

#[derive(Clone, Debug, Default)]
pub struct Item {
    pub name: OsString,              // file name, or "C:\" for a drive
    pub label: Option<OsString>,     // shown instead of name (drives)
    pub type_text: Option<OsString>, // type shown in the list; filled in by the file list (Type sort uses it)
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

pub struct Listing {
    pub location: Location,
    pub items: Vec<Item>,
    pub error: Option<String>,
    pub is_file: bool, // with an error: the path exists but is a file
}

/// Reads a folder. Blocking: call it off the UI thread.
pub fn list_folder(location: &Location, show_hidden: bool) -> Listing {
    let mut r = Listing { location: location.clone(), items: Vec::new(), error: None, is_file: false };
    let Location::Dir(dir) = location else {
        list_drives(&mut r);
        return r;
    };
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
                r.error = Some(e.message());
                r.is_file = exists(dir) && !is_directory(dir);
            }
            return r;
        }
    };
    loop {
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
        if unsafe { FindNextFileW(guard.0, &mut fd) }.is_err() {
            break;
        }
    }
    r
}

fn list_drives(r: &mut Listing) {
    let mask = unsafe { GetLogicalDrives() };
    for i in 0..26u8 {
        if mask & (1 << i) == 0 {
            continue;
        }
        let root = format!("{}:\\", (b'A' + i) as char);
        let root_w = wide(&root);
        let mut it = Item {
            name: root.into(),
            attrs: FILE_ATTRIBUTE_DIRECTORY.0,
            icon: -1,
            ..Default::default()
        };
        let mut sfi = SHFILEINFOW::default();
        let flags = SHGFI_DISPLAYNAME | SHGFI_TYPENAME | SHGFI_SYSICONINDEX | SHGFI_SMALLICON;
        if unsafe {
            SHGetFileInfoW(pcwstr(&root_w), FILE_FLAGS_AND_ATTRIBUTES(0), Some(&mut sfi), size_of::<SHFILEINFOW>() as u32, flags)
        } != 0
        {
            it.label = Some(from_wide(&sfi.szDisplayName));
            it.type_text = Some(from_wide(&sfi.szTypeName));
            it.icon = sfi.iIcon;
        }
        let (mut free, mut total) = (0u64, 0u64);
        if unsafe { GetDiskFreeSpaceExW(pcwstr(&root_w), Some(&mut free), Some(&mut total), None) }.is_ok() {
            let mut t = it.type_text.take().unwrap_or_default();
            t.push(format!("  \u{b7}  {} free of {}", byte_size(free).to_string_lossy(), byte_size(total).to_string_lossy()));
            it.type_text = Some(t);
        }
        r.items.push(it);
    }
}

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

/// Folders always first; names compare the way Explorer does ("file2" < "file10").
pub fn sort_items(items: &mut Vec<Item>, key: SortKey, ascending: bool) {
    let mut keyed: Vec<(Vec<u16>, Item)> = items.drain(..).map(|it| (wide(&it.name), it)).collect();
    keyed.sort_by(|(an, a), (bn, b)| {
        use std::cmp::Ordering;
        if a.is_dir() != b.is_dir() {
            return if a.is_dir() { Ordering::Less } else { Ordering::Greater };
        }
        let mut c = match key {
            SortKey::Modified => a.modified.cmp(&b.modified),
            SortKey::Size => a.size.cmp(&b.size),
            // By the type name shown in the list when known, else by extension.
            SortKey::Type => match (&a.type_text, &b.type_text) {
                (Some(at), Some(bt)) => unsafe { StrCmpLogicalW(pcwstr(&wide(at)), pcwstr(&wide(bt))) }.cmp(&0),
                _ => {
                    let r = unsafe { CompareStringOrdinal(ext(an), ext(bn), true) };
                    (r.0 - CSTR_EQUAL.0).cmp(&0)
                }
            },
            SortKey::Name => Ordering::Equal,
        };
        if c == Ordering::Equal {
            c = unsafe { StrCmpLogicalW(pcwstr(an), pcwstr(bn)) }.cmp(&0);
        }
        if ascending { c } else { c.reverse() }
    });
    items.extend(keyed.into_iter().map(|(_, it)| it));
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
pub fn extension_of(name: &OsStr) -> Vec<u16> {
    to_lower(&OsString::from_wide(ext(&wide(name))))
}

/// Case-insensitive substring match, or a wildcard match when the filter has * or ?.
/// `filter` is NUL-terminated.
pub fn matches_filter(item: &Item, filter: &[u16]) -> bool {
    if filter.len() <= 1 {
        return true;
    }
    let shown = wide(item.shown());
    unsafe {
        if filter.iter().any(|&c| c == b'*' as u16 || c == b'?' as u16) {
            PathMatchSpecW(pcwstr(&shown), pcwstr(filter)).as_bool()
        } else {
            !StrStrIW(pcwstr(&shown), pcwstr(filter)).is_null()
        }
    }
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

#[cfg(test)]
mod tests {
    use super::*;

    fn dir(p: &str) -> Location {
        Location::dir(p)
    }
    fn text(l: &Location) -> String {
        l.text().to_string_lossy().into_owned()
    }
    fn item(name: &str, size: u64, is_dir: bool) -> Item {
        Item { name: name.into(), size, attrs: if is_dir { FILE_ATTRIBUTE_DIRECTORY.0 } else { 0 }, ..Default::default() }
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
    }

    #[test]
    fn join_and_names() {
        assert_eq!(dir("C:\\").join(OsStr::new("a")), PathBuf::from("C:\\a"));
        assert_eq!(Location::ThisPc.join(OsStr::new("D:\\")), PathBuf::from("D:\\"));
        assert_eq!(dir("C:\\a\\b").display_name(), "b");
        assert_eq!(dir("C:\\").display_name(), "C:\\");
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
        assert!(resolve_argument(OsStr::new(".")).same(&Location::dir(cwd))); // "cubic.exe ." from a terminal
        assert_eq!(text(&resolve_argument(OsStr::new("C:\\Windows\\"))), "C:\\Windows");
        assert_eq!(text(&normalize(OsStr::new("\\\\server\\share\\"))), "\\\\server\\share");
        let windir = std::env::var_os("SystemRoot").unwrap();
        assert!(normalize(OsStr::new("%SystemRoot%")).same(&Location::dir(windir)));
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
        assert_eq!(extension_of(OsStr::new("Photo.JPG")), ".jpg".encode_utf16().collect::<Vec<_>>());
        assert!(extension_of(OsStr::new("README")).is_empty());
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
        sort_items(&mut v, SortKey::Type, true);
        assert_eq!(names(&v), "a,b,notes.md,file2.txt,file10.txt");
    }

    #[test]
    fn sorting_by_type_name() {
        let mut v = vec![item("b.txt", 0, false), item("a.zip", 0, false), item("c.docx", 0, false)];
        v[0].type_text = Some("Text Document".into());
        v[1].type_text = Some("Compressed (zipped) Folder".into());
        v[2].type_text = Some("Microsoft Word Document".into());
        sort_items(&mut v, SortKey::Type, true);
        assert_eq!(names(&v), "a.zip,c.docx,b.txt");
    }

    #[test]
    fn filtering() {
        let photo = item("Holiday Photo.jpg", 0, false);
        assert!(matches_filter(&photo, &wide("photo")));
        assert!(matches_filter(&photo, &wide("")));
        assert!(!matches_filter(&photo, &wide("video")));
        assert!(matches_filter(&item("report.PDF", 0, false), &wide("*.pdf")));
        assert!(!matches_filter(&item("report.pdf.txt", 0, false), &wide("*.pdf")));
    }

    #[test]
    fn listing() {
        let l = list_folder(&dir("C:\\Windows"), false);
        assert!(l.error.is_none());
        assert!(!l.items.is_empty());
        assert!(!l.items.iter().any(|i| i.name == "." || i.name == ".."));
        assert!(list_folder(&dir("C:\\definitely\\not\\here"), false).error.is_some());
        assert!(!list_folder(&Location::ThisPc, false).items.is_empty());
    }
}
