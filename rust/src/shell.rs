//! Everything that talks to the Windows shell. Windows itself copies, deletes, renames and shows the menus,
//! as in Explorer, so the app never re-implements (or gets wrong) any of that.

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::{OsStr, OsString, c_void};
use std::mem::ManuallyDrop;
use std::os::windows::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::Arc;

use windows::Win32::Foundation::{
    CloseHandle, E_FAIL, E_INVALIDARG, ERROR_CANCELLED, FILETIME, GlobalFree, HANDLE, HWND, LPARAM, LRESULT, MAX_PATH,
    POINT, POINTL, WPARAM,
};
use windows::Win32::Globalization::{CP_ACP, MULTI_BYTE_TO_WIDE_CHAR_FLAGS, MultiByteToWideChar};
use windows::Win32::Storage::FileSystem::{
    FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_NORMAL, FILE_FLAGS_AND_ATTRIBUTES, GetDriveTypeW, SearchPathW,
    WIN32_FIND_DATAW,
};
use windows::Win32::System::Com::{
    BIND_OPTS, CLSCTX_ALL, CLSCTX_INPROC_SERVER, COINIT_APARTMENTTHREADED, COINIT_DISABLE_OLE1DDE, CoCreateInstance,
    CoInitializeEx, CoTaskMemFree, CoUninitialize, CreateBindCtx, DVASPECT_CONTENT, FORMATETC, IBindCtx, IDataObject,
    IPersistFile, STGM_CREATE, STGM_READ, STGMEDIUM, STGMEDIUM_0, TYMED_HGLOBAL,
};
use windows::Win32::System::DataExchange::{
    CloseClipboard, EmptyClipboard, IsClipboardFormatAvailable, OpenClipboard, RegisterClipboardFormatW,
    SetClipboardData,
};
use windows::Win32::System::LibraryLoader::{GetProcAddress, LOAD_LIBRARY_SEARCH_SYSTEM32, LoadLibraryExW};
use windows::Win32::System::Memory::{GMEM_MOVEABLE, GlobalAlloc, GlobalLock, GlobalSize, GlobalUnlock};
use windows::Win32::System::Ole::{
    CF_HDROP, CF_UNICODETEXT, DROPEFFECT, DROPEFFECT_COPY, DROPEFFECT_LINK, DROPEFFECT_MOVE, DROPEFFECT_NONE,
    IDropSource, IDropTarget, IDropTarget_Impl, OleSetClipboard, ReleaseStgMedium,
};
use windows::Win32::System::SystemServices::{MK_RBUTTON, MODIFIERKEYS_FLAGS};
use windows::Win32::System::Threading::{
    CreateEventW, CreateThread, INFINITE, SetEvent, SetThreadStackGuarantee, THREAD_CREATION_FLAGS,
};
use windows::Win32::System::WindowsProgramming::{DRIVE_NO_ROOT_DIR, DRIVE_REMOTE};
use windows::Win32::UI::Controls::HIMAGELIST;
use windows::Win32::UI::Input::KeyboardAndMouse::{VK_CONTROL, VK_SHIFT};
use windows::Win32::UI::Shell::Common::ITEMIDLIST;
use windows::Win32::UI::Shell::{
    CFSTR_DROPDESCRIPTION, DROPDESCRIPTION, DROPFILES, DROPIMAGE_INVALID, DROPIMAGE_NOIMAGE, DROPIMAGETYPE,
    ASSOCF_OPEN_BYEXENAME, ASSOCSTR_EXECUTABLE, AssocQueryStringW,
    CMF_CANRENAME, CMF_EXTENDEDVERBS, CMF_NORMAL, CMIC_MASK_CONTROL_DOWN, CMIC_MASK_PTINVOKE, CMIC_MASK_SHIFT_DOWN,
    CMINVOKECOMMANDINFO, CMINVOKECOMMANDINFOEX, CLSID_DragDropHelper, FOF_ALLOWUNDO,
    FOF_NOCONFIRMATION, FOF_WANTNUKEWARNING, FOFX_RECYCLEONDELETE, FOLDERID_ComputerFolder, FileOperation,
    GCS_VERBW, IContextMenu, IContextMenu2, IContextMenu3, IDropTargetHelper, IFileOperation,
    IFileOperationProgressSink, IFileOperationProgressSink_Impl, IFileSystemBindData, IFileSystemBindData_Impl,
    ILFindLastID, ILIsEqual, IShellFolder,
    IShellItem, IShellLinkW, KF_FLAG_DEFAULT, STR_FILE_SYS_BIND_DATA, SLR_NO_UI, SLR_NOLINKINFO, SLR_NOSEARCH,
    SLR_NOTRACK, SLR_NOUPDATE, SEE_MASK_FLAG_NO_UI, SEE_MASK_NOASYNC,
    SHBindToParent, SHCNE_ATTRIBUTES, SHCNE_CREATE, SHCNE_DELETE, SHCNE_DRIVEADD,
    SHCNE_DRIVEREMOVED, SHCNE_MEDIAINSERTED, SHCNE_MEDIAREMOVED, SHCNE_MKDIR, SHCNE_RENAMEFOLDER,
    SHCNE_RENAMEITEM, SHCNE_RMDIR, SHCNE_UPDATEDIR, SHCNE_UPDATEITEM, SHCNRF_InterruptLevel, SHCNRF_NewDelivery,
    SHCNRF_ShellLevel, SHChangeNotification_Lock, SHChangeNotification_Unlock, SHChangeNotifyDeregister,
    SHChangeNotifyEntry, SHChangeNotifyRegister, SHCreateItemFromParsingName, SHDoDragDrop, SHELLEXECUTEINFOW,
    SHFILEINFOW, SHGFI_DISPLAYNAME, SHGFI_PIDL, SHGFI_SMALLICON, SHGFI_SYSICONINDEX, SHGFI_TYPENAME, SHGFI_USEFILEATTRIBUTES,
    SHGetDesktopFolder, SHGetFileInfoW, SHGetInstanceExplorer, SHGetKnownFolderIDList, SHGetKnownFolderPath,
    SHParseDisplayName, SHSetInstanceExplorer, ShellExecuteExW, ShellLink,
};
use windows::Win32::UI::WindowsAndMessaging::{
    AppendMenuW, CreatePopupMenu, DispatchMessageW, GetMenuItemCount, MF_GRAYED, MF_SEPARATOR, MF_STRING, MSG,
    MsgWaitForMultipleObjects, PM_REMOVE, PeekMessageW, PostMessageW, QS_ALLINPUT, SW_SHOWNORMAL, TPM_RETURNCMD,
    TPM_RIGHTBUTTON, TrackPopupMenuEx, TranslateMessage,
};
use windows::core::{
    ComObject, GUID, HRESULT, IUnknown, Interface, PCSTR, PCWSTR, PSTR, PWSTR, Ref, Result, implement, s, w,
};

use crate::folder::{
    Item, Location, SortKey, drop_back, drop_keys, exists, extension_of, is_directory, list_folder, normalize,
    sort_items,
};
use crate::util::{OwnedMenu, Pidl, copy_to, from_wide, key_down, pcwstr, wide};

const CMIC_MASK_UNICODE: u32 = 0x4000; // not in the windows crate
const FIRST_CMD: u32 = 1;
const LAST_CMD: u32 = 0x7FFF;
const OWN_FIRST: u32 = 0x8000; // the app's own items' ids, after the shell's

thread_local! {
    static TYPES: RefCell<HashMap<String, (i32, Arc<OsStr>)>> = RefCell::new(HashMap::new());
    // Set only while a context menu is on screen, for owner-drawn submenus.
    static OPEN_MENU: RefCell<(Option<IContextMenu2>, Option<IContextMenu3>)> = const { RefCell::new((None, None)) };
}

// ---------------------------------------------------------------- shell objects

/// The shell's id for a location; None when Windows can't parse it.
pub fn pidl_from(location: &Location) -> Option<Pidl> {
    unsafe {
        match location {
            Location::ThisPc => SHGetKnownFolderIDList(&FOLDERID_ComputerFolder, 0, None).ok().map(Pidl),
            Location::Dir(p) => {
                let mut raw: *mut ITEMIDLIST = std::ptr::null_mut();
                SHParseDisplayName(pcwstr(&wide(p)), None::<&IBindCtx>, &mut raw, 0, None).ok()?;
                Some(Pidl(raw))
            }
        }
    }
}

/// An interface (menu, data object, drop target...) on one item by its full path; covers This PC and \\server\share.
fn ui_object_at<T: Interface>(owner: HWND, location: &Location) -> Result<T> {
    let pidl = pidl_from(location).ok_or(E_FAIL)?;
    unsafe {
        let parent: IShellFolder = SHBindToParent(pidl.0, None)?;
        parent.GetUIObjectOf(owner, &[ILFindLastID(pidl.0) as *const _], None)
    }
}

/// The listing's find data, handed to the shell (STR_FILE_SYS_BIND_DATA) so parsing a name needs no disk query.
#[implement(IFileSystemBindData)]
struct FindData(RefCell<WIN32_FIND_DATAW>);

impl IFileSystemBindData_Impl for FindData_Impl {
    fn SetFindData(&self, fd: *const WIN32_FIND_DATAW) -> Result<()> {
        *self.0.borrow_mut() = unsafe { *fd };
        Ok(())
    }
    fn GetFindData(&self, fd: *mut WIN32_FIND_DATAW) -> Result<()> {
        unsafe { *fd = *self.0.borrow() };
        Ok(())
    }
}

/// A bind context that hands the shell what the returned find data holds. STGM_CREATE tells the shell to take
/// it as given instead of checking the disk.
fn find_data_bind() -> Result<(IFileSystemBindData, IBindCtx)> {
    let known: IFileSystemBindData = FindData(RefCell::new(WIN32_FIND_DATAW::default())).into();
    unsafe {
        let bind = CreateBindCtx(0)?;
        bind.SetBindOptions(&BIND_OPTS { cbStruct: size_of::<BIND_OPTS>() as u32, grfMode: STGM_CREATE.0, ..Default::default() })?;
        bind.RegisterObjectParam(STR_FILE_SYS_BIND_DATA, &known)?;
        Ok((known, bind))
    }
}

/// A folder as a shell folder, for its items or its background. The desktop itself (an empty pidl) can't be bound:
/// it is the desktop folder.
fn shell_folder(dir: &Location) -> Result<IShellFolder> {
    let pidl = pidl_from(dir).ok_or(E_FAIL)?;
    unsafe {
        let desktop = SHGetDesktopFolder()?;
        if (*pidl.0).mkid.cb == 0 { Ok(desktop) } else { desktop.BindToObject(pidl.0, None::<&IBindCtx>) }
    }
}

/// The same for listed items of one folder: opened once, each item parsed from its own find data. Full
/// paths took ~9 ms per file (90 s for 10,000), plain names a disk query each (spec 4.7).
fn ui_object_of<T: Interface>(owner: HWND, dir: &Location, items: &[Item]) -> Result<T> {
    match items {
        [] => return Err(E_INVALIDARG.into()),
        [item] => return ui_object_at(owner, &Location::dir(dir.join(&item.name))),
        _ => {}
    }
    unsafe {
        let folder = shell_folder(dir)?;
        let (known, bind) = find_data_bind()?;
        let mut owned: Vec<Pidl> = Vec::with_capacity(items.len());
        for it in items {
            let mut fd = WIN32_FIND_DATAW {
                dwFileAttributes: it.attrs,
                ftLastWriteTime: FILETIME { dwLowDateTime: it.modified as u32, dwHighDateTime: (it.modified >> 32) as u32 },
                nFileSizeHigh: (it.size >> 32) as u32,
                nFileSizeLow: it.size as u32,
                ..Default::default()
            };
            copy_to(fd.cFileName.as_mut_ptr(), MAX_PATH as i32, &it.name); // "C:\" for a drive under This PC
            known.SetFindData(&fd)?;
            let mut child: *mut ITEMIDLIST = std::ptr::null_mut();
            folder.ParseDisplayName(owner, &bind, pcwstr(&fd.cFileName), None, &mut child, std::ptr::null_mut())?;
            owned.push(Pidl(child));
        }
        let children: Vec<*const ITEMIDLIST> = owned.iter().map(|p| p.0 as *const _).collect();
        folder.GetUIObjectOf(owner, &children, None)
    }
}

/// Runs one IFileOperation: Windows shows progress, conflict and error dialogs, and records undo.
fn file_operation(owner: HWND, flags: u32, add: impl FnOnce(&IFileOperation) -> Result<()>) -> Result<()> {
    unsafe {
        let op: IFileOperation = CoCreateInstance(&FileOperation, None, CLSCTX_ALL)?;
        op.SetOwnerWindow(owner)?;
        op.SetOperationFlags(windows::Win32::UI::Shell::FILEOPERATION_FLAGS(flags))?;
        add(&op)?;
        op.PerformOperations()?;
        // The user answered No or Cancel: nothing (or not everything) was done.
        if op.GetAnyOperationsAborted().is_ok_and(|aborted| aborted.as_bool()) {
            return Err(ERROR_CANCELLED.to_hresult().into());
        }
        Ok(())
    }
}

// ---------------------------------------------------------------- icons and types

/// What the shell says about a path (or an extension probe with SHGFI_USEFILEATTRIBUTES): None when it fails.
pub(crate) fn file_info(path: &[u16], attrs: FILE_FLAGS_AND_ATTRIBUTES, flags: windows::Win32::UI::Shell::SHGFI_FLAGS) -> Option<SHFILEINFOW> {
    let mut sfi = SHFILEINFOW::default();
    let ok = unsafe { SHGetFileInfoW(pcwstr(path), attrs, Some(&mut sfi), size_of::<SHFILEINFOW>() as u32, flags) };
    (ok != 0).then_some(sfi)
}

pub fn system_image_list() -> HIMAGELIST {
    let mut sfi = SHFILEINFOW::default();
    let list = unsafe {
        SHGetFileInfoW(
            w!("x"),
            FILE_ATTRIBUTE_NORMAL,
            Some(&mut sfi),
            size_of::<SHFILEINFOW>() as u32,
            SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON,
        )
    };
    HIMAGELIST(list as _)
}

/// "Explorer": Explorer's look for a list or tree; "": Windows' plain look. uxtheme.dll is loaded here, not
/// imported: Windows looks for an imported DLL next to the exe first, before main() can restrict the search.
pub fn set_theme(control: HWND, name: &str) {
    unsafe {
        let Ok(ux) = LoadLibraryExW(w!("uxtheme.dll"), None, LOAD_LIBRARY_SEARCH_SYSTEM32) else { return };
        if let Some(set) = GetProcAddress(ux, s!("SetWindowTheme")) {
            let set: unsafe extern "system" fn(HWND, PCWSTR, PCWSTR) -> HRESULT = std::mem::transmute(set);
            let ids = if name.is_empty() { w!("") } else { PCWSTR::null() }; // "" for both: Windows' plain look
            let _ = set(control, pcwstr(&wide(name)), ids);
        }
    }
}

/// The cache key of an item's type: "\" for a folder, else its extension.
fn type_key(it: &Item) -> String {
    if it.is_dir() { "\\".to_string() } else { extension_of(&it.name) }
}

/// Icon and type name depend only on the extension: one shell call, with no disk access (but the type's icon handler).
fn ask_type(it: &Item) -> (i32, Arc<OsStr>) {
    let (probe, attrs) = if it.is_dir() {
        (wide("folder"), FILE_ATTRIBUTE_DIRECTORY)
    } else {
        (wide(format!("file{}", type_key(it))), FILE_ATTRIBUTE_NORMAL)
    };
    let flags = SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_TYPENAME;
    let (icon, text) = file_info(&probe, attrs, flags).map(|s| (s.iIcon, from_wide(&s.szTypeName))).unwrap_or_default();
    (icon, text.into()) // given to every item of the type (Item::type_text)
}

/// Cached for the UI thread: the folder icon, and the few items a load did not fill in (`fill_types`).
fn type_info(it: &Item) -> (i32, Arc<OsStr>) {
    let key = type_key(it);
    if let Some(found) = TYPES.with(|t| t.borrow().get(&key).cloned()) {
        return found;
    }
    let info = ask_type(it);
    TYPES.with(|t| t.borrow_mut().insert(key, info.clone()));
    info
}

/// Each item's type name and, unless it has an icon of its own, its icon, from the shell's answer for the extension,
/// asked once per extension. Worker thread: a file type's icon handler (a DLL some program installed) can hang, and
/// asked here it hangs the load, not the window.
pub fn fill_types(items: &mut [Item]) {
    let mut seen: HashMap<String, (i32, Arc<OsStr>)> = HashMap::new();
    for it in items.iter_mut() {
        if it.type_text.is_some() {
            continue; // a drive comes with its own
        }
        let (icon, text) = seen.entry(type_key(it)).or_insert_with(|| ask_type(it)).clone();
        it.type_text = Some(text);
        if !has_own_icon(it) {
            it.icon = icon;
        }
    }
}

/// .exe, .lnk and the like have their own icon, read on a worker thread (`read_own_icons`): it may need the network.
pub fn has_own_icon(it: &Item) -> bool {
    const OWN: [&str; 10] = [".exe", ".lnk", ".ico", ".url", ".cur", ".ani", ".msc", ".cpl", ".scr", ".appref-ms"];
    !it.is_dir() && OWN.contains(&extension_of(&it.name).as_str())
}

pub struct IconBatch {
    pub dir: Location,
    pub icons: Vec<(OsString, i32)>, // file name, icon
}

/// Worker thread: looks up the own icons of these files.
pub fn read_own_icons(dir: &Location, names: Vec<OsString>) -> IconBatch {
    let icons = names
        .into_iter()
        .filter_map(|name| {
            let sfi = file_info(&wide(dir.join(&name)), FILE_FLAGS_AND_ATTRIBUTES(0), SHGFI_SYSICONINDEX | SHGFI_SMALLICON)?;
            Some((name, sfi.iIcon))
        })
        .collect();
    IconBatch { dir: dir.clone(), icons }
}

/// The icon of an item's type. UI thread; never touches the disk.
pub fn type_icon(it: &Item) -> i32 {
    type_info(it).0
}

pub fn folder_icon() -> i32 {
    type_info(&Item { attrs: FILE_ATTRIBUTE_DIRECTORY.0, ..Default::default() }).0
}

pub fn type_name(it: &Item) -> Arc<OsStr> {
    it.type_text.clone().unwrap_or_else(|| type_info(it).1)
}

pub fn known_folder(id: &GUID) -> Option<PathBuf> {
    unsafe {
        let p = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, None).ok()?;
        let path = PathBuf::from(from_wide(p.as_wide()));
        CoTaskMemFree(Some(p.0 as *const _));
        Some(path)
    }
}

/// A shortcut's name and a folder or file-type icon, from the .lnk alone: its own icon can wait on a dead server.
pub fn describe_shortcut(lnk: &Path) -> (OsString, i32) {
    let (target, folder) = shortcut_target(lnk).map_or((None, false), |(t, folder)| (Some(t), folder));
    let name = file_info(&wide(lnk), FILE_FLAGS_AND_ATTRIBUTES(0), SHGFI_DISPLAYNAME)
        .map(|s| from_wide(&s.szDisplayName))
        .filter(|n| !n.is_empty())
        .unwrap_or_else(|| lnk.file_name().unwrap_or_default().to_os_string());
    let (probe, attrs) = match target {
        _ if folder => (wide("x"), FILE_ATTRIBUTE_DIRECTORY),
        Some(Location::Dir(t)) => (wide(t), FILE_ATTRIBUTE_NORMAL), // only the extension matters
        _ => (wide(lnk), FILE_ATTRIBUTE_NORMAL),
    };
    let icon = file_info(&probe, attrs, SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON).map_or(0, |s| s.iIcon);
    (name, icon)
}

/// Display name and icon of a location, as Explorer shows it.
pub fn describe(location: &Location) -> (OsString, i32) {
    let Some(pidl) = pidl_from(location) else { return (location.display_name(), folder_icon()) };
    let flags = SHGFI_PIDL | SHGFI_DISPLAYNAME | SHGFI_SYSICONINDEX | SHGFI_SMALLICON;
    let mut sfi = SHFILEINFOW::default();
    let ok = unsafe {
        SHGetFileInfoW(PCWSTR(pidl.0 as *const u16), FILE_FLAGS_AND_ATTRIBUTES(0), Some(&mut sfi), size_of::<SHFILEINFOW>() as u32, flags)
    };
    if ok == 0 { (location.display_name(), folder_icon()) } else { (from_wide(&sfi.szDisplayName), sfi.iIcon) }
}

// ---------------------------------------------------------------- menus

/// One item, given by its full path, as a shell data object (what copy, drag and drop hand around).
#[cfg(feature = "selftest")] // only the self-test uses it
pub fn data_object_at(owner: HWND, location: &Location) -> Option<IDataObject> {
    ui_object_at(owner, location).ok()
}

/// Context menu for items of one folder.
pub fn item_menu(owner: HWND, dir: &Location, items: &[Item]) -> Option<IContextMenu> {
    ui_object_of(owner, dir, items).ok()
}

/// Context menu for one item given by its full path (the tree's folders, This PC, a drive).
pub fn item_menu_at(owner: HWND, location: &Location) -> Option<IContextMenu> {
    ui_object_at(owner, location).ok()
}

pub fn background_menu(owner: HWND, dir: &Location) -> Option<IContextMenu> {
    unsafe { shell_folder(dir).ok()?.CreateViewObject(owner).ok() }
}

/// An item of the app's own at the top of a shell menu (Paste on a folder's menu, for one).
pub struct OwnItem<'a> {
    pub text: &'static str,
    pub enabled: bool,
    pub act: &'a dyn Fn(),
}

/// `intercept` sees the chosen verb first and returns true if it handled it. `own` items come first.
pub fn show_menu(owner: HWND, cm: &IContextMenu, pt: POINT, intercept: &mut dyn FnMut(&str) -> bool, own: &[OwnItem]) {
    let Ok(menu) = (unsafe { CreatePopupMenu() }) else { return };
    let menu = OwnedMenu(menu);
    unsafe {
        for (i, item) in own.iter().enumerate() {
            let state = if item.enabled { MF_STRING } else { MF_STRING | MF_GRAYED };
            let _ = AppendMenuW(menu.0, state, OWN_FIRST as usize + i, pcwstr(&wide(item.text)));
        }
        if !own.is_empty() {
            let _ = AppendMenuW(menu.0, MF_SEPARATOR, 0, PCWSTR::null());
        }
    }
    let mut flags = CMF_NORMAL | CMF_CANRENAME;
    if key_down(VK_SHIFT) {
        flags |= CMF_EXTENDEDVERBS;
    }
    unsafe {
        let first = GetMenuItemCount(Some(menu.0)).max(0) as u32;
        if cm.QueryContextMenu(menu.0, first, FIRST_CMD, LAST_CMD, flags).is_err() {
            return;
        }
        OPEN_MENU.with(|m| *m.borrow_mut() = (cm.cast().ok(), cm.cast().ok()));
        let id = TrackPopupMenuEx(menu.0, (TPM_RETURNCMD | TPM_RIGHTBUTTON).0, pt.x, pt.y, owner, None).0 as u32;
        OPEN_MENU.with(|m| *m.borrow_mut() = (None, None));
        if id >= OWN_FIRST && let Some(item) = own.get((id - OWN_FIRST) as usize) {
            return (item.act)();
        }
        if id < FIRST_CMD {
            return;
        }
        let mut verb = [0u16; 80];
        let verb = match cm.GetCommandString((id - FIRST_CMD) as usize, GCS_VERBW, None, PSTR(verb.as_mut_ptr().cast()), 80) {
            Ok(()) => from_wide(&verb).to_string_lossy().into_owned(),
            Err(_) => String::new(),
        };
        if intercept(&verb) {
            return;
        }
        let mut mask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        if key_down(VK_CONTROL) {
            mask |= CMIC_MASK_CONTROL_DOWN;
        }
        if key_down(VK_SHIFT) {
            mask |= CMIC_MASK_SHIFT_DOWN;
        }
        let ici = CMINVOKECOMMANDINFOEX {
            cbSize: size_of::<CMINVOKECOMMANDINFOEX>() as u32,
            fMask: mask,
            hwnd: owner,
            lpVerb: PCSTR((id - FIRST_CMD) as usize as *const u8),
            lpVerbW: PCWSTR((id - FIRST_CMD) as usize as *const u16),
            nShow: SW_SHOWNORMAL.0,
            ptInvoke: pt,
            ..Default::default()
        };
        let _ = cm.InvokeCommand(&ici as *const _ as *const CMINVOKECOMMANDINFO);
    }
}

/// Forwards owner-drawn submenu messages (Send to, Open with) while a menu is open.
pub fn forward_menu_message(msg: u32, wp: WPARAM, lp: LPARAM) -> Option<LRESULT> {
    let (m2, m3) = OPEN_MENU.with(|m| m.borrow().clone());
    unsafe {
        if let Some(m3) = m3 {
            let mut r = LRESULT(0);
            return m3.HandleMenuMsg2(msg, wp, lp, Some(&mut r)).ok().map(|_| r);
        }
        m2.and_then(|m2| m2.HandleMenuMsg(msg, wp, lp).ok().map(|_| LRESULT(0)))
    }
}

pub fn invoke_verb(owner: HWND, cm: Option<IContextMenu>, verb: &str) {
    let Some(cm) = cm else { return };
    let Ok(menu) = (unsafe { CreatePopupMenu() }) else { return };
    let menu = OwnedMenu(menu);
    let verb_a: Vec<u8> = verb.bytes().chain(Some(0)).collect();
    let verb_w = wide(verb);
    unsafe {
        if cm.QueryContextMenu(menu.0, 0, FIRST_CMD, LAST_CMD, CMF_NORMAL).is_err() {
            return;
        }
        let ici = CMINVOKECOMMANDINFOEX {
            cbSize: size_of::<CMINVOKECOMMANDINFOEX>() as u32,
            fMask: CMIC_MASK_UNICODE,
            hwnd: owner,
            lpVerb: PCSTR(verb_a.as_ptr()),
            lpVerbW: pcwstr(&verb_w),
            nShow: SW_SHOWNORMAL.0,
            ..Default::default()
        };
        let _ = cm.InvokeCommand(&ici as *const _ as *const CMINVOKECOMMANDINFO);
    }
}

// ---------------------------------------------------------------- file operations

/// False when the clipboard could not be set: a path too long for the shell, or another program holds it.
pub fn copy_to_clipboard(owner: HWND, dir: &Location, items: &[Item], cut: bool) -> bool {
    let Ok(obj) = ui_object_of::<IDataObject>(owner, dir, items) else { return false };
    // Tells the paste target whether to move (cut) or copy; a copy also allows Paste shortcut.
    let effect = if cut { DROPEFFECT_MOVE } else { DROPEFFECT_COPY | DROPEFFECT_LINK };
    set_data(&obj, unsafe { RegisterClipboardFormatW(w!("Preferred DropEffect")) }, &effect.0.to_ne_bytes());
    unsafe { OleSetClipboard(&obj).is_ok() }
}

/// Puts the bytes into a data object as `format`, which then owns them; false if it took nothing.
fn set_data(data: &IDataObject, format: u32, bytes: &[u8]) -> bool {
    let fmt = FORMATETC {
        cfFormat: format as u16,
        ptd: std::ptr::null_mut(),
        dwAspect: DVASPECT_CONTENT.0,
        lindex: -1,
        tymed: TYMED_HGLOBAL.0 as u32,
    };
    unsafe {
        let Ok(h) = GlobalAlloc(GMEM_MOVEABLE, bytes.len()) else { return false };
        let p = GlobalLock(h) as *mut u8;
        if p.is_null() {
            let _ = GlobalFree(Some(h));
            return false;
        }
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), p, bytes.len());
        let _ = GlobalUnlock(h);
        let mut med = STGMEDIUM {
            tymed: TYMED_HGLOBAL.0 as u32,
            u: STGMEDIUM_0 { hGlobal: h },
            pUnkForRelease: ManuallyDrop::new(None),
        };
        if data.SetData(&fmt, &med, true).is_ok() {
            return true;
        }
        ReleaseStgMedium(&mut med);
        false
    }
}

/// Same code path as Explorer's Paste (and Paste shortcut, with `shortcuts`), with Windows' progress UI.
pub fn paste_into(owner: HWND, dir: &Location, shortcuts: bool) {
    if matches!(dir, Location::Dir(_)) {
        invoke_verb(owner, background_menu(owner, dir), if shortcuts { "pastelink" } else { "paste" });
    }
}

/// The clipboard holds something Paste puts in a folder.
pub fn can_paste() -> bool {
    // Files, or items with no file path that Windows' Paste takes too: from inside a zip, on a phone, or attachments
    // another program offers as files.
    unsafe {
        [
            u32::from(CF_HDROP.0),
            RegisterClipboardFormatW(w!("Shell IDList Array")),
            RegisterClipboardFormatW(w!("FileGroupDescriptorW")),
        ]
        .into_iter()
        .any(|format| IsClipboardFormatAvailable(format).is_ok())
    }
}

/// False when nothing was deleted: the user said No to Windows' question, or it failed.
pub fn delete_items(owner: HWND, dir: &Location, items: &[Item], permanently: bool) -> bool {
    let Ok(obj) = ui_object_of::<IDataObject>(owner, dir, items) else { return false };
    // FOF_WANTNUKEWARNING always: Windows still asks before deleting what it cannot recycle (a share, a USB stick).
    let mut flags = FOF_WANTNUKEWARNING.0;
    if !permanently {
        flags |= FOF_ALLOWUNDO.0 | FOFX_RECYCLEONDELETE.0 | FOF_NOCONFIRMATION.0;
    }
    file_operation(owner, flags, |op| unsafe { op.DeleteItems(&obj) }).is_ok()
}

/// Hears the name a rename gave its item (rename_item).
#[implement(IFileOperationProgressSink)]
struct RenameSink(RefCell<Option<OsString>>); // None until Windows reports the rename done

impl IFileOperationProgressSink_Impl for RenameSink_Impl {
    fn PostRenameItem(&self, _: u32, _: Ref<IShellItem>, name: &PCWSTR, hr: HRESULT, _: Ref<IShellItem>) -> Result<()> {
        if hr.is_ok() && !name.is_null() {
            // The name itself: the new item's keeps any trailing spaces.
            *self.0.borrow_mut() = Some(from_wide(unsafe { name.as_wide() }));
        }
        Ok(())
    }
    // Nothing else is of interest here.
    fn StartOperations(&self) -> Result<()> {
        Ok(())
    }
    fn FinishOperations(&self, _: HRESULT) -> Result<()> {
        Ok(())
    }
    fn PreRenameItem(&self, _: u32, _: Ref<IShellItem>, _: &PCWSTR) -> Result<()> {
        Ok(())
    }
    fn PreMoveItem(&self, _: u32, _: Ref<IShellItem>, _: Ref<IShellItem>, _: &PCWSTR) -> Result<()> {
        Ok(())
    }
    fn PostMoveItem(
        &self,
        _: u32,
        _: Ref<IShellItem>,
        _: Ref<IShellItem>,
        _: &PCWSTR,
        _: HRESULT,
        _: Ref<IShellItem>,
    ) -> Result<()> {
        Ok(())
    }
    fn PreCopyItem(&self, _: u32, _: Ref<IShellItem>, _: Ref<IShellItem>, _: &PCWSTR) -> Result<()> {
        Ok(())
    }
    fn PostCopyItem(
        &self,
        _: u32,
        _: Ref<IShellItem>,
        _: Ref<IShellItem>,
        _: &PCWSTR,
        _: HRESULT,
        _: Ref<IShellItem>,
    ) -> Result<()> {
        Ok(())
    }
    fn PreDeleteItem(&self, _: u32, _: Ref<IShellItem>) -> Result<()> {
        Ok(())
    }
    fn PostDeleteItem(&self, _: u32, _: Ref<IShellItem>, _: HRESULT, _: Ref<IShellItem>) -> Result<()> {
        Ok(())
    }
    fn PreNewItem(&self, _: u32, _: Ref<IShellItem>, _: &PCWSTR) -> Result<()> {
        Ok(())
    }
    fn PostNewItem(
        &self,
        _: u32,
        _: Ref<IShellItem>,
        _: &PCWSTR,
        _: &PCWSTR,
        _: u32,
        _: HRESULT,
        _: Ref<IShellItem>,
    ) -> Result<()> {
        Ok(())
    }
    fn UpdateProgress(&self, _: u32, _: u32) -> Result<()> {
        Ok(())
    }
    fn ResetTimer(&self) -> Result<()> {
        Ok(())
    }
    fn PauseTimer(&self) -> Result<()> {
        Ok(())
    }
    fn ResumeTimer(&self) -> Result<()> {
        Ok(())
    }
}

/// The name the item has now, which Windows can make other than new_name: "b (2).txt" when b.txt is taken (it
/// asks first), and no trailing spaces. An error when it failed or the user said No.
pub fn rename_item(owner: HWND, path: &Path, new_name: &OsStr) -> Result<OsString> {
    unsafe {
        let item: IShellItem = SHCreateItemFromParsingName(pcwstr(&wide(path)), None::<&IBindCtx>)?;
        let name = wide(new_name);
        let sink = ComObject::new(RenameSink(RefCell::new(None)));
        let heard: IFileOperationProgressSink = sink.to_interface();
        file_operation(owner, FOF_ALLOWUNDO.0, |op| op.RenameItem(&item, pcwstr(&name), &heard))?;
        Ok(sink.0.take().unwrap_or_else(|| new_name.to_os_string())) // the name asked for if Windows said nothing
    }
}

pub fn open_file(owner: HWND, path: &Path) {
    let file = wide(path);
    let dir = wide(path.parent().unwrap_or(Path::new("")));
    let mut sei = SHELLEXECUTEINFOW {
        cbSize: size_of::<SHELLEXECUTEINFOW>() as u32,
        hwnd: owner,
        lpFile: pcwstr(&file),
        lpDirectory: pcwstr(&dir),
        nShow: SW_SHOWNORMAL.0,
        ..Default::default()
    };
    unsafe {
        let _ = ShellExecuteExW(&mut sei); // Windows shows its own "open with" / error dialog
    }
}

/// Explorer on the folder, or Windows Search scoped to it: what Qoqdir leaves to Explorer (thumbnails, searching
/// inside subfolders). The path goes into the search-ms: link as UTF-8 with everything but letters, digits and
/// -_.~ as %XX, the way such links are written ("C%3A%5CUsers").
pub fn open_in_explorer(owner: HWND, dir: &Path, search: bool) {
    let file = if search {
        let mut url = String::from("search-ms:crumb=location:");
        for b in dir.to_string_lossy().bytes() {
            if b.is_ascii_alphanumeric() || b"-_.~".contains(&b) { url.push(b as char) } else { url.push_str(&format!("%{b:02X}")) }
        }
        wide(url)
    } else {
        wide(dir)
    };
    let verb = wide("explore");
    let mut sei = SHELLEXECUTEINFOW {
        cbSize: size_of::<SHELLEXECUTEINFOW>() as u32,
        hwnd: owner,
        lpVerb: if search { PCWSTR::null() } else { pcwstr(&verb) },
        lpFile: pcwstr(&file),
        nShow: SW_SHOWNORMAL.0,
        ..Default::default()
    };
    unsafe {
        let _ = ShellExecuteExW(&mut sei);
    }
}

/// The full path App Paths gives for a program name ("chrome", "winword"), as the Run box finds one there, or
/// None. The key is the name with ".exe" when it carries no extension of its own.
fn app_path_program(name: &OsStr) -> Option<Vec<u16>> {
    let key = if extension_of(name).is_empty() { wide(format!("{}.exe", name.to_string_lossy())) } else { wide(name) };
    let mut out = vec![0u16; 1024];
    let mut n = out.len() as u32;
    let hr = unsafe {
        AssocQueryStringW(ASSOCF_OPEN_BYEXENAME, ASSOCSTR_EXECUTABLE, pcwstr(&key), PCWSTR::null(),
                          Some(PWSTR(out.as_mut_ptr())), &mut n)
    };
    if hr.is_err() {
        return None;
    }
    let len = out.iter().position(|&c| c == 0)?;
    out.truncate(len + 1); // keep the NUL
    Some(out)
}

/// The full path a bare name typed in the address bar runs, found the way the Run box finds one: the PATH, then
/// App Paths. Windows would also look in the folder shown, where a program of that name could be waiting (in
/// Downloads, on a USB stick), so a program name found in neither place comes back as None and is not run from
/// there (5.3). A path or web address (a slash or colon) and a document name (an extension outside PATHEXT,
/// "notes.txt") stay as typed: a document still opens the folder shown's own.
pub fn command_program(name: &OsStr) -> Option<Vec<u16>> {
    let typed = wide(name);
    if typed.iter().any(|&c| c == b'\\' as u16 || c == b'/' as u16 || c == b':' as u16) {
        return Some(typed);
    }
    let own = extension_of(name);
    let exts = match std::env::var("PATHEXT") {
        Ok(v) if !v.is_empty() => v.to_lowercase(),
        _ => ".com;.exe;.bat;.cmd".to_string(), // unset: the usual few
    };
    if !own.is_empty() && !exts.split(';').any(|e| e == own) {
        return Some(typed); // a document, not a program
    }
    // Folder by folder along the PATH, each tried with every extension, as cmd and the Run box go: a .cmd early on the
    // PATH wins over an .exe of that name further on.
    let wanted: Vec<Vec<u16>> =
        exts.split(';').filter(|e| !e.is_empty() && (own.is_empty() || *e == own)).map(wide).collect();
    let mut found = vec![0u16; 32768];
    for dir in std::env::split_paths(&std::env::var_os("PATH").unwrap_or_default()) {
        if dir.as_os_str().is_empty() {
            continue;
        }
        let dir = wide(&dir);
        for ext in &wanted {
            let n = unsafe { SearchPathW(pcwstr(&dir), pcwstr(&typed), pcwstr(ext), Some(&mut found), None) } as usize;
            if n > 0 && n < found.len() {
                found.truncate(n + 1); // with its NUL
                return Some(found);
            }
        }
    }
    app_path_program(name)
}

/// Runs what was typed in the address bar: "cmd", "notepad file.txt", a web address...
pub fn run_command(owner: HWND, command: &OsStr, work_dir: Option<&Path>) -> bool {
    let dir = work_dir.map(wide);
    let run = |file: Option<Vec<u16>>, args: Option<&[u16]>| {
        let Some(file) = file else { return false }; // on neither PATH nor App Paths: not run from the folder (5.3)
        let mut sei = SHELLEXECUTEINFOW {
            cbSize: size_of::<SHELLEXECUTEINFOW>() as u32,
            fMask: SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC, // NOASYNC: it runs on a worker that ends right after
            hwnd: owner,
            lpFile: pcwstr(&file),
            lpParameters: args.map_or(PCWSTR::null(), pcwstr),
            lpDirectory: dir.as_deref().map_or(PCWSTR::null(), pcwstr),
            nShow: SW_SHOWNORMAL.0,
            ..Default::default()
        };
        unsafe { ShellExecuteExW(&mut sei).is_ok() }
    };
    let text = command.to_string_lossy();
    if run(command_program(command), None) {
        return true;
    }
    match text.split_once(' ') {
        Some((file, args)) => run(command_program(OsStr::new(file)), Some(&wide(args))),
        None => false,
    }
}

/// What Ctrl+` starts in a folder (5.3): Windows Terminal ("wt -d ."), found as a typed name is, or else cmd. Finding
/// it reads the PATH, so both run on a worker.
pub struct Terminal {
    pub program: Vec<u16>, // wide, as command_program gives it
    pub args: &'static str,
}

pub fn terminal_program() -> Option<Terminal> {
    if let Some(wt) = command_program(OsStr::new("wt")) {
        return Some(Terminal { program: wt, args: "-d ." }); // "." is its folder
    }
    command_program(OsStr::new("cmd")).map(|cmd| Terminal { program: cmd, args: "" })
}

pub fn start_terminal(owner: HWND, terminal: &Terminal, dir: &Path) -> bool {
    let (args, dir) = (wide(terminal.args), wide(dir));
    let mut sei = SHELLEXECUTEINFOW {
        cbSize: size_of::<SHELLEXECUTEINFOW>() as u32,
        fMask: SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC, // NOASYNC: it runs on a worker that ends right after
        hwnd: owner,
        lpFile: pcwstr(&terminal.program),
        lpParameters: if terminal.args.is_empty() { PCWSTR::null() } else { pcwstr(&args) },
        lpDirectory: pcwstr(&dir),
        nShow: SW_SHOWNORMAL.0,
        ..Default::default()
    };
    unsafe { ShellExecuteExW(&mut sei).is_ok() }
}

/// The clipboard then holds just this text, as it is: a name can hold a lone surrogate, which no str can.
pub fn copy_text(owner: HWND, text: &OsStr) -> bool {
    let text: Vec<u16> = text.encode_wide().chain(std::iter::once(0)).collect();
    unsafe {
        let Ok(h) = GlobalAlloc(GMEM_MOVEABLE, text.len() * size_of::<u16>()) else { return false };
        let p = GlobalLock(h) as *mut u16;
        if p.is_null() {
            let _ = GlobalFree(Some(h));
            return false;
        }
        std::ptr::copy_nonoverlapping(text.as_ptr(), p, text.len());
        let _ = GlobalUnlock(h);
        // Another program can hold it for a moment: then it fails, as Ctrl+C would.
        let mut ok = OpenClipboard(Some(owner)).is_ok();
        if ok {
            ok = EmptyClipboard().is_ok() && SetClipboardData(u32::from(CF_UNICODETEXT.0), Some(HANDLE(h.0))).is_ok();
            let _ = CloseClipboard();
        }
        if !ok {
            let _ = GlobalFree(Some(h)); // once set, the clipboard owns it
        }
        ok
    }
}

pub fn drag_out(owner: HWND, dir: &Location, items: &[Item]) {
    if let Ok(obj) = ui_object_of::<IDataObject>(owner, dir, items) {
        let effects = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        let _ = unsafe { SHDoDragDrop(Some(owner), &obj, None::<&IDropSource>, effects) };
    }
}

// ---------------------------------------------------------------- shortcuts

/// A .lnk file's target, normalized like every way into a tab (a stored "D:\photos\" would give a blank caption and a
/// stuck Up): a path, This PC, or nothing (another file, a web link, a library), and whether it was a folder. A target
/// on a drive letter that is gone is looked for where the shortcut says it is relative to itself (a flash drive on
/// another letter). No disk is asked: a server that is off costs nothing.
pub fn shortcut_target(lnk: &Path) -> Option<(Location, bool)> {
    if extension_of(lnk.as_os_str()) != ".lnk" {
        return None;
    }
    unsafe {
        let link: IShellLinkW = CoCreateInstance(&ShellLink, None, CLSCTX_INPROC_SERVER).ok()?;
        let file: IPersistFile = link.cast().ok()?;
        file.Load(pcwstr(&wide(lnk)), STGM_READ).ok()?;
        let mut target = vec![0u16; 32768]; // not MAX_PATH: a folder deep in a network share has a longer path
        let mut stored = WIN32_FIND_DATAW::default(); // the target as it was when the shortcut was made
        if link.GetPath(&mut target, &mut stored, 0).is_ok() && target[0] != 0 {
            let folder = stored.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY.0 != 0;
            // A target on a drive letter that is gone (a flash drive that came back under another letter) is looked for
            // where the shortcut says it is relative to itself: Windows stores that for one made on the target's drive.
            // No search, no box, and no disk is asked: a drive that is there is left to the loader, since a sleeping
            // disk or a server that is off would freeze the UI thread (a drag over the tree asks here).
            let on_drive = target[1] == u16::from(b':') && target[2] == u16::from(b'\\');
            if on_drive
                && GetDriveTypeW(pcwstr(&[target[0], target[1], target[2], 0])) == DRIVE_NO_ROOT_DIR
                && link.Resolve(HWND::default(), (SLR_NO_UI | SLR_NOSEARCH | SLR_NOTRACK | SLR_NOLINKINFO | SLR_NOUPDATE).0 as u32).is_ok()
            {
                let _ = link.GetPath(&mut target, std::ptr::null_mut(), 0); // the target where it is now
            }
            return Some((normalize(&from_wide(&target)), folder));
        }
        // No path: This PC is the one such target this app can show itself; Windows opens the rest.
        let pidl = Pidl(link.GetIDList().ok().filter(|p| !p.is_null())?);
        let pc = pidl_from(&Location::ThisPc)?;
        ILIsEqual(pidl.0, pc.0).as_bool().then_some((Location::ThisPc, true))
    }
}

/// Saved tabs: shortcuts in `dir` (Bookmarks\Tabs), numbered in tab order ("1 logs.lnk"); This PC too.
/// Saving replaces the shortcuts there and leaves anything else. Worker thread: a folder may be on a server
/// that is off.
pub fn save_folder_shortcuts(dir: &Path, folders: &[Location]) -> bool {
    // One save at a time: closing the window saves here and now while Ctrl+S's worker may still be writing (a folder
    // on a server that is off is slow), and the two would leave a mix of both sets. The later one waits and wins.
    static SAVING: std::sync::Mutex<()> = std::sync::Mutex::new(());
    let _one = SAVING.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
    let _ = std::fs::create_dir(dir); // fails harmlessly if it exists
    for it in list_folder(&Location::dir(dir), true, None).items {
        if !it.is_dir() && extension_of(&it.name) == ".lnk" {
            let _ = std::fs::remove_file(dir.join(&it.name));
        }
    }
    let mut ok = true;
    for (i, folder) in folders.iter().enumerate() {
        let mut file = OsString::from(format!("{} ", i + 1));
        file.push(shortcut_name(folder));
        ok &= write_folder_shortcut(&dir.join(&file), folder).is_ok();
    }
    ok
}

/// A bookmark: a shortcut to `folder` in `dir` (Bookmarks), named after the folder; one that is there already stays.
pub fn bookmark_folder(dir: &Path, folder: &Location) -> bool {
    // The shortcut's name comes from the folder's display name, so two folders can want the same one (D:\work\logs
    // and E:\logs both "logs.lnk"). Keep an existing one that already points here (bookmarking again), but give a
    // different folder the next free "name (2).lnk", as New folder numbers a clash, rather than claim it did.
    let base = shortcut_name(folder);
    let stem = Path::new(&base).file_stem().map_or_else(|| base.clone(), |s| s.to_os_string());
    for n in 1..=99 {
        let name = if n == 1 { base.clone() } else { OsString::from(format!("{} ({n}).lnk", stem.to_string_lossy())) };
        let lnk = dir.join(&name);
        if !exists(&lnk) {
            return write_folder_shortcut(&lnk, folder).is_ok();
        }
        if let Some((target, true)) = shortcut_target(&lnk)
            && target.same(folder)
        {
            return true; // already bookmarked
        }
    }
    false
}

/// A shortcut to `folder` at `lnk`, made from the path and find data that say "folder": the shortcut then says so
/// too, even for a folder that can't be reached now.
fn write_folder_shortcut(lnk: &Path, folder: &Location) -> Result<()> {
    unsafe {
        let pidl = match folder {
            Location::ThisPc => pidl_from(folder).ok_or(E_FAIL)?,
            Location::Dir(path) => {
                let (known, bind) = find_data_bind()?;
                let mut fd = WIN32_FIND_DATAW { dwFileAttributes: FILE_ATTRIBUTE_DIRECTORY.0, ..Default::default() };
                copy_to(fd.cFileName.as_mut_ptr(), MAX_PATH as i32, folder.display_name());
                known.SetFindData(&fd)?;
                let mut raw: *mut ITEMIDLIST = std::ptr::null_mut();
                SHParseDisplayName(pcwstr(&wide(path)), &bind, &mut raw, 0, None)?;
                Pidl(raw)
            }
        };
        let link: IShellLinkW = CoCreateInstance(&ShellLink, None, CLSCTX_INPROC_SERVER)?;
        link.SetIDList(pidl.0)?;
        link.cast::<IPersistFile>()?.Save(pcwstr(&wide(lnk)), true)
    }
}

/// The shortcut's file name for a folder: its display name, without what a file name can't hold.
fn shortcut_name(folder: &Location) -> OsString {
    let clean: Vec<u16> =
        wide(folder.display_name()).into_iter().filter(|c| !r#"\/:*?"<>|"#.encode_utf16().any(|b| b == *c)).collect();
    let mut name = from_wide(&clean);
    name.push(".lnk");
    name
}

/// The folders the saved tabs' shortcuts lead to, in order. Worker thread.
pub fn saved_folders(dir: &Path) -> Vec<Location> {
    let mut listing = list_folder(&Location::dir(dir), true, None);
    sort_items(&mut listing.items, SortKey::Name, true);
    listing
        .items
        .iter()
        .filter_map(|it| shortcut_target(&dir.join(&it.name)))
        .filter_map(|(target, folder)| folder.then_some(target))
        .collect()
}

// ---------------------------------------------------------------- background work

/// Runs `work` on a thread of its own, with COM set up for the shell calls it makes. The thread is never joined, so
/// it can outlive the window and the App: `work` captures values and a window handle only, never a reference to
/// either, and reports with post(), which drops the message when the window is gone.
///
/// A Windows thread, as the C++ version starts: std::thread::spawn builds its start-up anew for each caller, 11 of them,
/// about 20 KB of the exe.
pub fn in_background(work: impl FnOnce() + Send + 'static) {
    unsafe extern "system" fn run(job: *mut c_void) -> u32 {
        let work = *unsafe { Box::from_raw(job as *mut Box<dyn FnOnce() + Send>) };
        let mut guard = 64 * 1024; // room for the crash log if this worker overflows its stack (as the main thread has)
        unsafe {
            let _ = SetThreadStackGuarantee(&mut guard);
            let co = CoInitializeEx(None, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            work();
            if co.is_ok() {
                CoUninitialize();
            }
        }
        0
    }
    let job: Box<Box<dyn FnOnce() + Send>> = Box::new(Box::new(work));
    let job = Box::into_raw(job) as *const c_void;
    match unsafe { CreateThread(None, 0, Some(run), Some(job), THREAD_CREATION_FLAGS(0), None) } {
        Ok(thread) => unsafe {
            let _ = CloseHandle(thread);
        },
        // As std::thread::spawn did: the reply the caller waits for would never come.
        Err(e) => panic!("can't start a worker thread: {e}"),
    }
}

/// Hands `object` to a window (its handle as a number: a thread only posts to it) by a posted message; the handler
/// then owns it, through `Box::from_raw`. Freed here if the window is gone.
pub fn post<T>(wnd: usize, msg: u32, wp: usize, object: Box<T>) {
    let raw = Box::into_raw(object);
    if unsafe { PostMessageW(Some(HWND(wnd as _)), msg, WPARAM(wp), LPARAM(raw as isize)) }.is_err() {
        drop(unsafe { Box::from_raw(raw) });
    }
}

// ---------------------------------------------------------------- folder watch

/// Posts `msg` to `owner` when something in `dir` changes, or `dir` itself goes; stops when dropped.
pub struct FolderWatch {
    ids: [u32; 2],
    location: Option<Location>, // none: Windows refused it (the next load tries again)
}

impl FolderWatch {
    pub fn new(owner: HWND, msg: u32, dir: &Location) -> FolderWatch {
        let watch = |loc: &Location, events: i32| -> u32 {
            let Some(pidl) = pidl_from(loc) else { return 0 };
            let entry = SHChangeNotifyEntry { pidl: pidl.0, fRecursive: false.into() };
            let sources = SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery;
            unsafe { SHChangeNotifyRegister(owner, sources, events, msg, 1, &entry) }
        };
        let contents = SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR | SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER
            | SHCNE_UPDATEITEM | SHCNE_UPDATEDIR | SHCNE_ATTRIBUTES | SHCNE_DRIVEADD | SHCNE_DRIVEREMOVED
            | SHCNE_MEDIAINSERTED | SHCNE_MEDIAREMOVED;
        let itself = SHCNE_RMDIR | SHCNE_RENAMEFOLDER | SHCNE_DRIVEREMOVED | SHCNE_MEDIAREMOVED;
        let mut ids = [watch(dir, contents.0 as i32), 0];
        if let Location::Dir(_) = dir {
            ids[1] = watch(&dir.parent(), itself.0 as i32);
        }
        FolderWatch { ids, location: (ids[0] != 0).then(|| dir.clone()) }
    }

    pub fn watches(&self, location: &Location) -> bool {
        self.location.as_ref().is_some_and(|l| l.same(location))
    }

    /// Its folder is in `location`, which it watches too, for that folder going.
    pub fn watches_child_of(&self, location: &Location) -> bool {
        self.location.as_ref().is_some_and(|l| matches!(l, Location::Dir(_)) && l.parent().same(location))
    }
}

impl Drop for FolderWatch {
    fn drop(&mut self) {
        for id in self.ids {
            if id != 0 {
                unsafe {
                    let _ = SHChangeNotifyDeregister(id);
                }
            }
        }
    }
}

pub fn acknowledge_change(wp: WPARAM, lp: LPARAM) {
    unsafe {
        let lock = SHChangeNotification_Lock(HANDLE(wp.0 as _), lp.0 as u32, None, None);
        if !lock.is_invalid() {
            let _ = SHChangeNotification_Unlock(lock);
        }
    }
}

// ---------------------------------------------------------------- drag and drop

/// The local folder a tree node or a shortcut in the list stands for when files are dropped on it: its own, or the
/// shortcut's target (read from the .lnk alone). None for This PC, a link to a file or a web page, a shortcut whose
/// folder is gone, and anything on the network: asking Windows about a server that is off would freeze the drag.
pub fn local_folder(location: Location, shortcut: bool) -> Option<Location> {
    let path = location.path()?.to_path_buf(); // This PC: none
    let location = if shortcut {
        match shortcut_target(&path)? {
            (target, true) => target,
            _ => return None, // a link to a file
        }
    } else {
        location
    };
    let w: Vec<u16> = location.path()?.as_os_str().encode_wide().collect();
    let local = w.len() >= 3
        && w[1] == u16::from(b':')
        && unsafe { GetDriveTypeW(pcwstr(&[w[0], w[1], w[2], 0])) } != DRIVE_REMOTE;
    // A shortcut still says folder when its folder is gone: only looking tells, which costs no more than the drop
    // target does, as it reads that folder next.
    let local = local && (!shortcut || location.path().is_some_and(is_directory));
    local.then_some(location)
}

/// The folder a drop goes to, for the cursor (None: refused); called with None when the drag leaves or ends. The
/// second argument has the paths of the files the drag carries, read from its data: wherever it started, the drop
/// target itself refuses a folder they would go back to (4.8), and the closure can use them to show that, as the
/// tree does.
pub type DropTargetFn = Box<dyn Fn(Option<POINT>, &[Location]) -> Option<Location>>;

/// The paths of the files a drag carries, read once as it enters. None for items without one (from a zip folder or a
/// phone), and none for a drag that can only make shortcuts (a drive from This PC): a shortcut may go anywhere.
fn dragged_paths(data: Option<&IDataObject>, allowed: DROPEFFECT) -> Vec<Location> {
    let mut paths = Vec::new();
    let fmt = FORMATETC {
        cfFormat: CF_HDROP.0,
        ptd: std::ptr::null_mut(),
        dwAspect: DVASPECT_CONTENT.0,
        lindex: -1,
        tymed: TYMED_HGLOBAL.0 as u32,
    };
    let Some(data) = data.filter(|_| (allowed.0 & (DROPEFFECT_COPY | DROPEFFECT_MOVE).0) != 0) else { return paths };
    unsafe {
        let Ok(mut med) = data.GetData(&fmt) else { return paths };
        // The names one after another, read straight (DragQueryFileW(i) starts from the first name each time), and no
        // further than the memory goes: another program wrote them.
        let size = GlobalSize(med.u.hGlobal);
        let drop = if size > size_of::<DROPFILES>() { GlobalLock(med.u.hGlobal) as *const u8 } else { std::ptr::null() };
        if !drop.is_null() {
            let files = std::ptr::read_unaligned(drop.cast::<DROPFILES>());
            let start = files.pFiles as usize;
            if start < size {
                let bytes = std::slice::from_raw_parts(drop.add(start), size - start);
                // An older program's drag has them in ANSI, in the code page: made UTF-16 in one go, nuls and all.
                let text: Vec<u16> = if files.fWide.as_bool() {
                    bytes.as_chunks::<2>().0.iter().map(|&b| u16::from_le_bytes(b)).collect()
                } else {
                    let plain = MULTI_BYTE_TO_WIDE_CHAR_FLAGS(0);
                    let n = MultiByteToWideChar(CP_ACP, plain, bytes, None);
                    let mut wide = vec![0; usize::try_from(n).unwrap_or(0)];
                    MultiByteToWideChar(CP_ACP, plain, bytes, Some(&mut wide));
                    wide
                };
                let mut names = &text[..];
                // Each ends in a nul; an empty one ends the list, and one cut short is left out.
                while let Some(end) = names.iter().position(|&c| c == 0).filter(|&end| end > 0) {
                    paths.push(normalize(&from_wide(&names[..end])));
                    names = &names[end + 1..];
                }
            }
            let _ = GlobalUnlock(med.u.hGlobal);
        }
        ReleaseStgMedium(&mut med);
    }
    paths
}

#[implement(IDropTarget)]
struct DropTarget {
    owner: HWND,
    target: DropTargetFn,
    data: RefCell<Option<IDataObject>>,
    dragged: RefCell<Rc<[Location]>>,     // the paths of the files it carries (dragged_paths)
    dir: RefCell<Option<Location>>,       // the folder the drop currently goes to
    folder: RefCell<Option<IDropTarget>>, // and its shell drop target
    helper: Option<IDropTargetHelper>,
    described: Cell<bool>,                // the drag's tip is ours (describe)
}

pub fn new_drop_target(owner: HWND, target: DropTargetFn) -> IDropTarget {
    let helper = unsafe { CoCreateInstance(&CLSID_DragDropHelper, None, CLSCTX_INPROC_SERVER).ok() };
    DropTarget {
        owner,
        target,
        data: RefCell::new(None),
        dragged: RefCell::new(Rc::from([])),
        dir: RefCell::new(None),
        folder: RefCell::new(None),
        helper,
        described: Cell::new(false),
    }
    .into()
}

impl DropTarget {
    /// Aims the drop at the folder under the cursor; true if it changed (that folder has been told already).
    fn aim(&self, keys: MODIFIERKEYS_FLAGS, pt: &POINTL, effect: *mut DROPEFFECT) -> bool {
        let dragged = self.dragged.borrow().clone();
        let dir = (self.target)(Some(POINT { x: pt.x, y: pt.y }), &dragged)
            .filter(|d| drop_back(d, &dragged).is_none()); // never back where the files are, whoever asks (4.8)
        // Compared exactly, as the C++ version does: a folder can hold both "a" and "A".
        let unchanged = match (&dir, &*self.dir.borrow()) {
            (Some(a), Some(b)) => a.path().map(Path::as_os_str) == b.path().map(Path::as_os_str),
            (None, None) => true,
            _ => false,
        };
        if unchanged {
            return false;
        }
        let old = self.folder.borrow_mut().take();
        let data = self.data.borrow().clone();
        unsafe {
            if let Some(old) = old {
                let _ = old.DragLeave();
            }
            let folder = dir.as_ref().and_then(|d| ui_object_at::<IDropTarget>(self.owner, d).ok());
            // A plain drag that could move files reaches the folder as a right-button drag: Windows asks (5.8).
            let keys = drop_keys(keys, *effect);
            let entered = folder.filter(|f| f.DragEnter(data.as_ref(), keys, *pt, effect).is_ok());
            if entered.is_none() {
                *effect = DROPEFFECT_NONE;
            }
            *self.folder.borrow_mut() = entered;
        }
        *self.dir.borrow_mut() = dir;
        true
    }

    fn finish(&self) {
        (self.target)(None, &[]);
        let folder = self.folder.borrow_mut().take();
        if let Some(folder) = folder {
            unsafe {
                let _ = folder.DragLeave();
            }
        }
        *self.dir.borrow_mut() = None;
        self.clear_tip();
        *self.data.borrow_mut() = None;
        *self.dragged.borrow_mut() = Rc::from([]);
    }

    /// A drop with these keys asks with Windows' menu, and could move the files: a plain drag passed on as a
    /// right-button one (drop_keys), or a right-drag (5.8).
    fn asks(keys: MODIFIERKEYS_FLAGS, allowed: DROPEFFECT) -> bool {
        (drop_keys(keys, allowed).0 & MK_RBUTTON.0) != 0 && (allowed.0 & DROPEFFECT_MOVE.0) != 0
    }

    /// While a drop would ask, the drag's tip says "Copy or move to" its folder, not the folder's own "Move to", and
    /// shows no icon: none of Windows' icons says that a menu comes (5.8).
    fn describe(&self, ask: bool, effect: DROPEFFECT) {
        let into = self.dir.borrow().clone().filter(|_| self.folder.borrow().is_some());
        match into {
            Some(dir) if effect != DROPEFFECT_NONE && ask => {
                self.set_tip(DROPIMAGE_NOIMAGE, "Copy or move to %1", &dir.display_name());
            }
            _ => self.clear_tip(),
        }
    }

    /// Takes ours away, so the folder's own tip shows again.
    fn clear_tip(&self) {
        if self.described.get() {
            self.set_tip(DROPIMAGE_INVALID, "", OsStr::new(""));
        }
    }

    /// Puts a tip in the drag's data for its image to show; DROPIMAGE_INVALID takes ours away.
    fn set_tip(&self, kind: DROPIMAGETYPE, message: &str, insert: &OsStr) {
        self.described.set(false);
        let Some(data) = self.data.borrow().clone() else { return };
        // Filled apart and moved in whole: the struct is packed, so its fields can't be borrowed.
        let (mut text, mut name) = ([0u16; 260], [0u16; 260]);
        unsafe {
            copy_to(text.as_mut_ptr(), text.len() as i32, message);
            copy_to(name.as_mut_ptr(), name.len() as i32, insert);
        }
        let tip = DROPDESCRIPTION { r#type: kind, szMessage: text, szInsert: name };
        let bytes = unsafe { std::slice::from_raw_parts((&raw const tip).cast::<u8>(), size_of::<DROPDESCRIPTION>()) };
        let format = unsafe { RegisterClipboardFormatW(CFSTR_DROPDESCRIPTION) };
        self.described.set(set_data(&data, format, bytes) && kind != DROPIMAGE_INVALID);
    }
}

impl IDropTarget_Impl for DropTarget_Impl {
    fn DragEnter(&self, data: Ref<IDataObject>, keys: MODIFIERKEYS_FLAGS, pt: &POINTL, effect: *mut DROPEFFECT) -> Result<()> {
        *self.data.borrow_mut() = data.as_ref().cloned();
        let allowed = unsafe { *effect };
        *self.dragged.borrow_mut() = dragged_paths(data.as_ref(), allowed).into();
        if !self.aim(keys, pt, effect) {
            unsafe { *effect = DROPEFFECT_NONE }; // still no folder (each drag starts with none)
        }
        self.describe(DropTarget::asks(keys, allowed), unsafe { *effect });
        unsafe {
            if let Some(h) = &self.helper {
                let _ = h.DragEnter(self.owner, data.as_ref(), &POINT { x: pt.x, y: pt.y }, *effect);
            }
        }
        Ok(())
    }

    fn DragOver(&self, keys: MODIFIERKEYS_FLAGS, pt: &POINTL, effect: *mut DROPEFFECT) -> Result<()> {
        let ask = DropTarget::asks(keys, unsafe { *effect });
        if !ask {
            self.clear_tip(); // ours goes first: the folder puts its own
        }
        // Over a new folder, aim() has already asked it; otherwise the same folder decides again.
        if !self.aim(keys, pt, effect) {
            let folder = self.folder.borrow().clone();
            if folder.is_none_or(|f| unsafe { f.DragOver(drop_keys(keys, *effect), *pt, effect) }.is_err()) {
                unsafe { *effect = DROPEFFECT_NONE };
            }
        }
        self.describe(ask, unsafe { *effect });
        unsafe {
            if let Some(h) = &self.helper {
                let _ = h.DragOver(&POINT { x: pt.x, y: pt.y }, *effect);
            }
        }
        Ok(())
    }

    fn DragLeave(&self) -> Result<()> {
        unsafe {
            if let Some(h) = &self.helper {
                let _ = h.DragLeave();
            }
        }
        self.finish();
        Ok(())
    }

    fn Drop(&self, data: Ref<IDataObject>, keys: MODIFIERKEYS_FLAGS, pt: &POINTL, effect: *mut DROPEFFECT) -> Result<()> {
        unsafe {
            if let Some(h) = &self.helper {
                let _ = h.Drop(data.as_ref(), &POINT { x: pt.x, y: pt.y }, *effect);
            }
        }
        let folder = self.folder.borrow_mut().take(); // it gets the drop, not a DragLeave
        self.finish(); // the drag is over (e.g. remove highlights)
        if folder.is_none_or(|f| unsafe { f.Drop(data.as_ref(), drop_keys(keys, *effect), *pt, effect) }.is_err()) {
            unsafe { *effect = DROPEFFECT_NONE };
        }
        Ok(())
    }
}

// ---------------------------------------------------------------- shell process

/// A hold on the process for a worker that must finish what it writes (saved tabs, a recycle) even when the window
/// closes: main waits until it is let go (wait_for_shell). None once the app is closing.
pub fn hold_process() -> Option<ProcessHold> {
    unsafe { SHGetInstanceExplorer() }.ok().map(|unknown| ProcessHold(unknown.into_raw()))
}

/// The ShellRef main made, as Windows' own copies take it; let go when dropped.
pub struct ProcessHold(*mut c_void);

// ShellRef is agile (IAgileObject): any thread may let go of it.
unsafe impl Send for ProcessHold {}

impl Drop for ProcessHold {
    fn drop(&mut self) {
        drop(unsafe { IUnknown::from_raw(self.0) });
    }
}

/// Keeps the process alive while Windows finishes copies and Properties dialogs started here (`done`: all released).
#[implement(windows::Win32::System::Com::IAgileObject)]
struct ShellRef {
    done: HANDLE,
}

impl windows::Win32::System::Com::IAgileObject_Impl for ShellRef_Impl {} // marker interface, no methods

impl Drop for ShellRef {
    fn drop(&mut self) {
        unsafe {
            let _ = SetEvent(self.done);
        }
    }
}

pub struct ShellProcessRef {
    obj: Option<IUnknown>,
    done: HANDLE,
}

impl ShellProcessRef {
    pub fn new() -> ShellProcessRef {
        let done = unsafe { CreateEventW(None, true, false, None) }.unwrap_or_default();
        let obj: IUnknown = ShellRef { done }.into();
        unsafe { SHSetInstanceExplorer(&obj) };
        ShellProcessRef { obj: Some(obj), done }
    }

    /// After the message loop: waits, still pumping messages, until Windows has released every reference.
    pub fn wait_for_shell(&mut self) {
        unsafe { SHSetInstanceExplorer(None::<&IUnknown>) };
        self.obj = None;
        if self.done.is_invalid() {
            return;
        }
        loop {
            let r = unsafe { MsgWaitForMultipleObjects(Some(&[self.done]), false, INFINITE, QS_ALLINPUT) };
            if r.0 != 1 {
                break; // WAIT_OBJECT_0 + 1 means "messages waiting"; anything else: done (or failed)
            }
            let mut msg = MSG::default();
            while unsafe { PeekMessageW(&mut msg, None, 0, 0, PM_REMOVE) }.as_bool() {
                unsafe {
                    let _ = TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        }
    }
}

impl Drop for ShellProcessRef {
    fn drop(&mut self) {
        unsafe {
            if !self.done.is_invalid() {
                let _ = CloseHandle(self.done);
            }
        }
    }
}
