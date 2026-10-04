//! Everything that talks to the Windows shell. Copying, deleting, renaming,
//! context menus and icons are done by Windows itself, exactly as in Explorer,
//! so the app never re-implements (and never gets wrong) any of that.
//! COM objects are released automatically when dropped.

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::{OsStr, OsString};
use std::mem::ManuallyDrop;
use std::path::{Path, PathBuf};

use windows::Win32::Foundation::{
    CloseHandle, E_FAIL, E_INVALIDARG, ERROR_CANCELLED, FILETIME, HANDLE, HWND, LPARAM, LRESULT, MAX_PATH, POINT, POINTL,
    WPARAM,
};
use windows::Win32::Storage::FileSystem::{
    FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_NORMAL, FILE_FLAGS_AND_ATTRIBUTES, WIN32_FIND_DATAW,
};
use windows::Win32::System::Com::{
    BIND_OPTS, CLSCTX_ALL, CLSCTX_INPROC_SERVER, CoCreateInstance, CoTaskMemFree, CreateBindCtx, DVASPECT_CONTENT,
    FORMATETC, IBindCtx, IDataObject, IPersistFile, STGM_CREATE, STGM_READ, STGMEDIUM, STGMEDIUM_0, TYMED_HGLOBAL,
};
use windows::Win32::System::DataExchange::{IsClipboardFormatAvailable, RegisterClipboardFormatW};
use windows::Win32::System::Memory::{GMEM_MOVEABLE, GlobalAlloc, GlobalLock, GlobalUnlock};
use windows::Win32::System::Ole::{
    CF_HDROP, DROPEFFECT, DROPEFFECT_COPY, DROPEFFECT_LINK, DROPEFFECT_MOVE, DROPEFFECT_NONE, IDropSource, IDropTarget,
    IDropTarget_Impl, OleSetClipboard, ReleaseStgMedium,
};
use windows::Win32::System::SystemServices::MODIFIERKEYS_FLAGS;
use windows::Win32::System::Threading::{CreateEventW, INFINITE, SetEvent};
use windows::Win32::UI::Controls::HIMAGELIST;
use windows::Win32::UI::Input::KeyboardAndMouse::{GetKeyState, VK_CONTROL, VK_SHIFT};
use windows::Win32::UI::Shell::Common::ITEMIDLIST;
use windows::Win32::UI::Shell::{
    CMF_CANRENAME, CMF_EXTENDEDVERBS, CMF_NORMAL, CMIC_MASK_CONTROL_DOWN, CMIC_MASK_PTINVOKE, CMIC_MASK_SHIFT_DOWN,
    CMINVOKECOMMANDINFO, CMINVOKECOMMANDINFOEX, CLSID_DragDropHelper, FOF_ALLOWUNDO,
    FOF_NOCONFIRMATION, FOF_WANTNUKEWARNING, FOFX_RECYCLEONDELETE, FOLDERID_ComputerFolder, FileOperation,
    GCS_VERBW, IContextMenu, IContextMenu2, IContextMenu3, IDropTargetHelper, IFileOperation,
    IFileOperationProgressSink, IFileSystemBindData, IFileSystemBindData_Impl, ILFindLastID, ILIsEqual, IShellFolder,
    IShellItem, IShellLinkW, KF_FLAG_DEFAULT, STR_FILE_SYS_BIND_DATA,
    SEE_MASK_FLAG_NO_UI, SHBindToParent, SHCNE_ATTRIBUTES, SHCNE_CREATE, SHCNE_DELETE, SHCNE_DRIVEADD,
    SHCNE_DRIVEREMOVED, SHCNE_MEDIAINSERTED, SHCNE_MEDIAREMOVED, SHCNE_MKDIR, SHCNE_RENAMEFOLDER,
    SHCNE_RENAMEITEM, SHCNE_RMDIR, SHCNE_UPDATEDIR, SHCNE_UPDATEITEM, SHCNRF_InterruptLevel, SHCNRF_NewDelivery,
    SHCNRF_ShellLevel, SHChangeNotification_Lock, SHChangeNotification_Unlock, SHChangeNotifyDeregister,
    SHChangeNotifyEntry, SHChangeNotifyRegister, SHCreateItemFromParsingName, SHDoDragDrop, SHELLEXECUTEINFOW,
    SHFILEINFOW, SHGFI_SMALLICON, SHGFI_SYSICONINDEX, SHGFI_TYPENAME, SHGFI_USEFILEATTRIBUTES,
    SHGetDesktopFolder, SHGetFileInfoW, SHGetKnownFolderIDList, SHGetKnownFolderPath, SHParseDisplayName,
    SHSetInstanceExplorer, ShellExecuteExW, ShellLink,
};
use windows::Win32::UI::WindowsAndMessaging::{
    AppendMenuW, CreatePopupMenu, DispatchMessageW, GetMenuItemCount, MF_GRAYED, MF_SEPARATOR, MF_STRING, MSG,
    MsgWaitForMultipleObjects, PM_REMOVE, PeekMessageW, QS_ALLINPUT, SW_SHOWNORMAL, TPM_RETURNCMD, TPM_RIGHTBUTTON,
    TrackPopupMenuEx, TranslateMessage,
};
use windows::core::{GUID, IUnknown, Interface, PCSTR, PCWSTR, PSTR, Ref, Result, implement, w};

use crate::folder::{Item, Location, extension_of};
use crate::util::{OwnedMenu, Pidl, from_wide, pcwstr, wide};

const CMIC_MASK_UNICODE: u32 = 0x4000; // not in the windows crate
const FIRST_CMD: u32 = 1;
const LAST_CMD: u32 = 0x7FFF;
const PASTE_CMD: u32 = 0x8000; // our own items, after the shell's ids
const PASTE_SHORTCUT_CMD: u32 = 0x8001;

thread_local! {
    static TYPES: RefCell<HashMap<String, (i32, OsString)>> = RefCell::new(HashMap::new());
    static OWN_ICONS: RefCell<HashMap<PathBuf, i32>> = RefCell::new(HashMap::new()); // full path -> icon
    // Set only while a context menu is on screen, for owner-drawn submenus.
    static OPEN_MENU: RefCell<(Option<IContextMenu2>, Option<IContextMenu3>)> = const { RefCell::new((None, None)) };
}

fn file_info(path: &[u16], attrs: FILE_FLAGS_AND_ATTRIBUTES, flags: windows::Win32::UI::Shell::SHGFI_FLAGS) -> Option<SHFILEINFOW> {
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

/// Icon and type name depend only on the extension, so ask the shell once per
/// extension. SHGFI_USEFILEATTRIBUTES means no disk access: it never blocks.
fn type_info(it: &Item) -> (i32, OsString) {
    let key = if it.is_dir() { "\\".to_string() } else { extension_of(&it.name) };
    if let Some(found) = TYPES.with(|t| t.borrow().get(&key).cloned()) {
        return found;
    }
    let (probe, attrs) =
        if it.is_dir() { (wide("folder"), FILE_ATTRIBUTE_DIRECTORY) } else { (wide(format!("file{key}")), FILE_ATTRIBUTE_NORMAL) };
    let flags = SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_TYPENAME;
    let info = file_info(&probe, attrs, flags).map(|s| (s.iIcon, from_wide(&s.szTypeName))).unwrap_or_default();
    TYPES.with(|t| t.borrow_mut().insert(key, info.clone()));
    info
}

/// Files like .exe and .lnk carry their own icon, which needs a disk (maybe
/// network) lookup. That runs on a worker thread (`read_own_icons`); until it
/// arrives, the file shows its type's icon.
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

/// UI thread: keeps the icons so they show from now on (also after refreshes).
pub fn remember_icons(batch: &IconBatch) {
    OWN_ICONS.with(|cache| {
        let mut cache = cache.borrow_mut();
        for (name, icon) in &batch.icons {
            cache.insert(batch.dir.join(name), *icon);
        }
    });
}

/// UI thread. Never touches the disk.
pub fn icon_for(path: &Path, it: &Item) -> i32 {
    if has_own_icon(it) {
        if let Some(icon) = OWN_ICONS.with(|cache| cache.borrow().get(path).copied()) {
            return icon;
        }
    }
    type_info(it).0
}

pub fn folder_icon() -> i32 {
    type_info(&Item { attrs: FILE_ATTRIBUTE_DIRECTORY.0, ..Default::default() }).0
}

pub fn type_name(it: &Item) -> OsString {
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

/// How a shortcut looks before its own icon is read: Windows' name for it ("Work", not "Work.lnk"), and the
/// icon of a folder or of the target's file type. From the shortcut file alone: asking Windows for the
/// target's own icon waits on a server that is off.
pub fn describe_shortcut(lnk: &Path) -> (OsString, i32) {
    use windows::Win32::UI::Shell::SHGFI_DISPLAYNAME;
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
    use windows::Win32::UI::Shell::{SHGFI_DISPLAYNAME, SHGFI_PIDL};
    let Some(pidl) = pidl_from(location) else { return (location.display_name(), folder_icon()) };
    let flags = SHGFI_PIDL | SHGFI_DISPLAYNAME | SHGFI_SYSICONINDEX | SHGFI_SMALLICON;
    let mut sfi = SHFILEINFOW::default();
    let ok = unsafe {
        SHGetFileInfoW(PCWSTR(pidl.0 as *const u16), FILE_FLAGS_AND_ATTRIBUTES(0), Some(&mut sfi), size_of::<SHFILEINFOW>() as u32, flags)
    };
    if ok == 0 { (location.display_name(), folder_icon()) } else { (from_wide(&sfi.szDisplayName), sfi.iIcon) }
}

fn pidl_from(location: &Location) -> Option<Pidl> {
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

/// Asks the shell for an interface on one item given by its full path (a context menu, a
/// data object, a drop target...). Resolving the full path exactly also covers items without
/// an ordinary parent folder: This PC, and a network share's top (\\server\share).
fn ui_object_at<T: Interface>(owner: HWND, location: &Location) -> Result<T> {
    let pidl = pidl_from(location).ok_or(E_FAIL)?;
    unsafe {
        let parent: IShellFolder = SHBindToParent(pidl.0, None)?;
        parent.GetUIObjectOf(owner, &[ILFindLastID(pidl.0) as *const _], None)
    }
}

/// What the folder listing already said about an item, handed to the shell through a bind
/// context (STR_FILE_SYS_BIND_DATA) so that parsing the item's name needs no disk access.
/// Without it the shell asks the disk about every item: one round trip each on a network
/// folder, tens of seconds for 10,000 selected files.
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

/// The same for items of one folder, as the list read them. The folder is opened once and each
/// item is looked up by name inside it, from its own find data: resolving every full path from
/// the desktop down took ~9 ms per file (90 s for 10,000 files), and even the name lookup cost
/// one disk query per item (the 1.5-2.5 s of spec 4.7 on an SSD, far more on a network folder).
fn ui_object_of<T: Interface>(owner: HWND, dir: &Location, items: &[Item]) -> Result<T> {
    match items {
        [] => return Err(E_INVALIDARG.into()),
        [item] => return ui_object_at(owner, &Location::dir(dir.join(&item.name))),
        _ => {}
    }
    unsafe {
        let dir_pidl = pidl_from(dir).ok_or(E_FAIL)?;
        let desktop = SHGetDesktopFolder()?;
        let folder: IShellFolder =
            if (*dir_pidl.0).mkid.cb == 0 { desktop } else { desktop.BindToObject(dir_pidl.0, None::<&IBindCtx>)? };
        // STGM_CREATE tells the shell to take the find data as given instead of checking the disk.
        let known: IFileSystemBindData = FindData(RefCell::new(WIN32_FIND_DATAW::default())).into();
        let bind = CreateBindCtx(0)?;
        bind.SetBindOptions(&BIND_OPTS { cbStruct: size_of::<BIND_OPTS>() as u32, grfMode: STGM_CREATE.0, ..Default::default() })?;
        bind.RegisterObjectParam(STR_FILE_SYS_BIND_DATA, &known)?;
        let mut owned: Vec<Pidl> = Vec::with_capacity(items.len());
        for it in items {
            let mut fd = WIN32_FIND_DATAW {
                dwFileAttributes: it.attrs,
                ftLastWriteTime: FILETIME { dwLowDateTime: it.modified as u32, dwHighDateTime: (it.modified >> 32) as u32 },
                nFileSizeHigh: (it.size >> 32) as u32,
                nFileSizeLow: it.size as u32,
                ..Default::default()
            };
            for (to, from) in fd.cFileName.iter_mut().zip(wide(&it.name).into_iter().take(MAX_PATH as usize - 1)) {
                *to = from; // "C:\" for a drive under This PC
            }
            known.SetFindData(&fd)?;
            let mut child: *mut ITEMIDLIST = std::ptr::null_mut();
            folder.ParseDisplayName(owner, &bind, PCWSTR(fd.cFileName.as_ptr()), None, &mut child, std::ptr::null_mut())?;
            owned.push(Pidl(child));
        }
        let children: Vec<*const ITEMIDLIST> = owned.iter().map(|p| p.0 as *const _).collect();
        folder.GetUIObjectOf(owner, &children, None)
    }
}

/// One item by its full path, as a shell data object (what copy, drag and drop hand around). The file
/// list's own items go through `ui_object_of` (copy, delete, drag), which builds them from the listing.
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
    let pidl = pidl_from(dir)?;
    unsafe {
        let desktop = SHGetDesktopFolder().ok()?;
        let folder: IShellFolder = desktop.BindToObject(pidl.0, None::<&IBindCtx>).ok()?;
        folder.CreateViewObject(owner).ok()
    }
}

fn key_down(vk: windows::Win32::UI::Input::KeyboardAndMouse::VIRTUAL_KEY) -> bool {
    unsafe { GetKeyState(vk.0 as i32) < 0 }
}

/// Shows a context menu. `intercept` sees the chosen verb first and returns
/// true if it handled it (e.g. "rename", or "open" on a folder). With `paste`, Paste and
/// Paste shortcut come first: a folder's own menu has neither (Explorer adds its own).
/// `paste` is called with true for Paste shortcut.
pub fn show_menu(owner: HWND, cm: &IContextMenu, pt: POINT, intercept: &mut dyn FnMut(&str) -> bool, paste: Option<&dyn Fn(bool)>) {
    let Ok(menu) = (unsafe { CreatePopupMenu() }) else { return };
    let menu = OwnedMenu(menu);
    if paste.is_some() {
        // Greyed when no files were copied.
        let copied = unsafe { IsClipboardFormatAvailable(u32::from(CF_HDROP.0)) }.is_ok();
        let state = if copied { MF_STRING } else { MF_STRING | MF_GRAYED };
        unsafe {
            let _ = AppendMenuW(menu.0, state, PASTE_CMD as usize, w!("Paste\tCtrl+V"));
            let _ = AppendMenuW(menu.0, state, PASTE_SHORTCUT_CMD as usize, w!("Paste shortcut"));
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
        if let (PASTE_CMD | PASTE_SHORTCUT_CMD, Some(paste)) = (id, paste) {
            return paste(id == PASTE_SHORTCUT_CMD);
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

pub fn invoke_verb(owner: HWND, cm: Option<IContextMenu>, verb: &str) -> bool {
    let Some(cm) = cm else { return false };
    let Ok(menu) = (unsafe { CreatePopupMenu() }) else { return false };
    let menu = OwnedMenu(menu);
    let verb_a: Vec<u8> = verb.bytes().chain(Some(0)).collect();
    let verb_w = wide(verb);
    unsafe {
        if cm.QueryContextMenu(menu.0, 0, FIRST_CMD, LAST_CMD, CMF_NORMAL).is_err() {
            return false;
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
        cm.InvokeCommand(&ici as *const _ as *const CMINVOKECOMMANDINFO).is_ok()
    }
}

/// False when the files could not be put on the clipboard: Windows could not describe them (a path too
/// long for the shell) or another program holds the clipboard open.
pub fn copy_to_clipboard(owner: HWND, dir: &Location, items: &[Item], cut: bool) -> bool {
    let Ok(obj) = ui_object_of::<IDataObject>(owner, dir, items) else { return false };
    unsafe {
        // Tells the paste target whether to move (cut) or copy.
        let fmt = FORMATETC {
            cfFormat: RegisterClipboardFormatW(w!("Preferred DropEffect")) as u16,
            ptd: std::ptr::null_mut(),
            dwAspect: DVASPECT_CONTENT.0,
            lindex: -1,
            tymed: TYMED_HGLOBAL.0 as u32,
        };
        if let Ok(h) = GlobalAlloc(GMEM_MOVEABLE, size_of::<u32>()) {
            let p = GlobalLock(h) as *mut u32;
            if !p.is_null() {
                *p = if cut { DROPEFFECT_MOVE.0 } else { (DROPEFFECT_COPY | DROPEFFECT_LINK).0 }; // a copy also allows Paste shortcut
                let _ = GlobalUnlock(h);
            }
            let mut med = STGMEDIUM {
                tymed: TYMED_HGLOBAL.0 as u32,
                u: STGMEDIUM_0 { hGlobal: h },
                pUnkForRelease: ManuallyDrop::new(None),
            };
            if obj.SetData(&fmt, &med, true).is_err() {
                ReleaseStgMedium(&mut med);
            }
        }
        OleSetClipboard(&obj).is_ok()
    }
}

/// Same code path as Explorer's Paste (and Paste shortcut, with `shortcuts`), with Windows' progress UI.
pub fn paste_into(owner: HWND, dir: &Location, shortcuts: bool) {
    if matches!(dir, Location::Dir(_)) {
        invoke_verb(owner, background_menu(owner, dir), if shortcuts { "pastelink" } else { "paste" });
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

/// False when nothing was deleted: the user said No to Windows' question, or it failed.
pub fn delete_items(owner: HWND, dir: &Location, items: &[Item], permanently: bool) -> bool {
    let Ok(obj) = ui_object_of::<IDataObject>(owner, dir, items) else { return false };
    // FOF_WANTNUKEWARNING in both cases: Windows still asks before destroying an item it cannot recycle
    // (network share, USB stick, oversized file); without it FOF_NOCONFIRMATION answers Yes for the user.
    let mut flags = FOF_WANTNUKEWARNING.0 as u32;
    if !permanently {
        flags |= FOF_ALLOWUNDO.0 as u32 | FOFX_RECYCLEONDELETE.0 as u32 | FOF_NOCONFIRMATION.0 as u32;
    }
    file_operation(owner, flags, |op| unsafe { op.DeleteItems(&obj) }).is_ok()
}

pub fn rename_item(owner: HWND, path: &Path, new_name: &OsStr) -> Result<()> {
    unsafe {
        let item: IShellItem = SHCreateItemFromParsingName(pcwstr(&wide(path)), None::<&IBindCtx>)?;
        let name = wide(new_name);
        file_operation(owner, FOF_ALLOWUNDO.0 as u32, |op| {
            op.RenameItem(&item, pcwstr(&name), None::<&IFileOperationProgressSink>)
        })
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

/// Runs what was typed in the address bar: "cmd", "notepad file.txt", a web address...
pub fn run_command(owner: HWND, command: &OsStr, work_dir: Option<&Path>) -> bool {
    let dir = work_dir.map(wide);
    let run = |file: &[u16], args: Option<&[u16]>| {
        let mut sei = SHELLEXECUTEINFOW {
            cbSize: size_of::<SHELLEXECUTEINFOW>() as u32,
            fMask: SEE_MASK_FLAG_NO_UI,
            hwnd: owner,
            lpFile: pcwstr(file),
            lpParameters: args.map_or(PCWSTR::null(), pcwstr),
            lpDirectory: dir.as_deref().map_or(PCWSTR::null(), pcwstr),
            nShow: SW_SHOWNORMAL.0,
            ..Default::default()
        };
        unsafe { ShellExecuteExW(&mut sei).is_ok() }
    };
    let text = command.to_string_lossy();
    if run(&wide(command), None) {
        return true;
    }
    match text.split_once(' ') {
        Some((file, args)) => run(&wide(file), Some(&wide(args))),
        None => false,
    }
}

pub fn drag_out(owner: HWND, dir: &Location, items: &[Item]) {
    if let Ok(obj) = ui_object_of::<IDataObject>(owner, dir, items) {
        let effects = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        let _ = unsafe { SHDoDragDrop(Some(owner), &obj, None::<&IDropSource>, effects) };
    }
}

/// The target stored in a shortcut: a path (as stored: normalize it before use), This PC, or nothing (a
/// web link, a library); and whether the target was a folder when the shortcut was made (false if not
/// recorded). Reads only the shortcut file, never the target, so a shortcut to a server that is off costs nothing.
pub fn shortcut_target(lnk: &Path) -> Option<(Location, bool)> {
    unsafe {
        let link: IShellLinkW = CoCreateInstance(&ShellLink, None, CLSCTX_INPROC_SERVER).ok()?;
        let file: IPersistFile = link.cast().ok()?;
        file.Load(pcwstr(&wide(lnk)), STGM_READ).ok()?;
        let mut target = vec![0u16; 32768]; // not MAX_PATH: a folder deep in a network share has a longer path
        let mut stored = WIN32_FIND_DATAW::default(); // the target as it was when the shortcut was made
        if link.GetPath(&mut target, &mut stored, 0).is_ok() && target[0] != 0 {
            let folder = stored.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY.0 != 0;
            return Some((Location::Dir(PathBuf::from(from_wide(&target))), folder));
        }
        // No path: This PC is the one such target this app can show itself; Windows opens the rest.
        let pidl = Pidl(link.GetIDList().ok().filter(|p| !p.is_null())?);
        let pc = pidl_from(&Location::ThisPc)?;
        ILIsEqual(pidl.0, pc.0).as_bool().then_some((Location::ThisPc, true))
    }
}

/// Posts `msg` to `owner` when something inside `dir` changes, or when `dir`
/// itself is deleted or renamed (seen from its parent). Stops when dropped.
pub struct FolderWatch {
    ids: [u32; 2],
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
        FolderWatch { ids }
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
        let mut pidls: *mut *mut ITEMIDLIST = std::ptr::null_mut();
        let mut event = 0i32;
        let lock = SHChangeNotification_Lock(HANDLE(wp.0 as _), lp.0 as u32, Some(&mut pidls), Some(&mut event));
        if !lock.is_invalid() {
            let _ = SHChangeNotification_Unlock(lock);
        }
    }
}

/// Picks the folder a drop goes to, from the cursor position (screen coordinates;
/// None = refuse). Called with None when the drag leaves or ends, so it can remove
/// any highlight it showed.
pub type DropTargetFn = Box<dyn Fn(Option<POINT>) -> Option<Location>>;

#[implement(IDropTarget)]
struct DropTarget {
    owner: HWND,
    target: DropTargetFn,
    data: RefCell<Option<IDataObject>>,
    dir: RefCell<Option<Location>>,       // the folder the drop currently goes to
    folder: RefCell<Option<IDropTarget>>, // and its shell drop target
    helper: Option<IDropTargetHelper>,
}

pub fn new_drop_target(owner: HWND, target: DropTargetFn) -> IDropTarget {
    let helper = unsafe { CoCreateInstance(&CLSID_DragDropHelper, None, CLSCTX_INPROC_SERVER).ok() };
    DropTarget { owner, target, data: RefCell::new(None), dir: RefCell::new(None), folder: RefCell::new(None), helper }
        .into()
}

impl DropTarget {
    /// Points the drop at the folder under the cursor. Returns true if that changed
    /// (then the new folder has already been told about the drag).
    fn aim(&self, keys: MODIFIERKEYS_FLAGS, pt: &POINTL, effect: *mut DROPEFFECT) -> bool {
        let dir = (self.target)(Some(POINT { x: pt.x, y: pt.y }));
        let unchanged = match (&dir, &*self.dir.borrow()) {
            (Some(a), Some(b)) => a.same(b),
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
        (self.target)(None);
        let folder = self.folder.borrow_mut().take();
        if let Some(folder) = folder {
            unsafe {
                let _ = folder.DragLeave();
            }
        }
        *self.dir.borrow_mut() = None;
        *self.data.borrow_mut() = None;
    }
}

impl IDropTarget_Impl for DropTarget_Impl {
    fn DragEnter(&self, data: Ref<IDataObject>, keys: MODIFIERKEYS_FLAGS, pt: &POINTL, effect: *mut DROPEFFECT) -> Result<()> {
        *self.data.borrow_mut() = data.as_ref().cloned();
        self.aim(keys, pt, effect);
        unsafe {
            if let Some(h) = &self.helper {
                let _ = h.DragEnter(self.owner, data.as_ref(), &POINT { x: pt.x, y: pt.y }, *effect);
            }
        }
        Ok(())
    }

    fn DragOver(&self, keys: MODIFIERKEYS_FLAGS, pt: &POINTL, effect: *mut DROPEFFECT) -> Result<()> {
        // Over a new folder, aim() has already asked it; otherwise the same folder decides again.
        if !self.aim(keys, pt, effect) {
            let folder = self.folder.borrow().clone();
            if folder.is_none_or(|f| unsafe { f.DragOver(keys, *pt, effect) }.is_err()) {
                unsafe { *effect = DROPEFFECT_NONE };
            }
        }
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
        let folder = self.folder.borrow_mut().take();
        (self.target)(None); // the drag is over (e.g. remove highlights)
        unsafe {
            if let Some(h) = &self.helper {
                let _ = h.Drop(data.as_ref(), &POINT { x: pt.x, y: pt.y }, *effect);
            }
            if folder.is_none_or(|f| f.Drop(data.as_ref(), keys, *pt, effect).is_err()) {
                *effect = DROPEFFECT_NONE;
            }
        }
        *self.dir.borrow_mut() = None;
        *self.data.borrow_mut() = None;
        Ok(())
    }
}

/// Keeps the process alive while Windows finishes background copies (and
/// Properties dialogs) started from the app. Windows holds references to this
/// object; when the last one goes away, `done` is signaled.
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

    /// Call after the message loop ends: waits (still pumping messages) until
    /// Windows has released every reference.
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
