//! Small helpers: UTF-16 strings for Windows, and owners that free Windows
//! resources automatically when they go out of scope.

use std::ffi::{OsStr, OsString};
use std::os::windows::ffi::{OsStrExt, OsStringExt};

use windows::Win32::Foundation::{HWND, LPARAM, WPARAM};
use windows::Win32::Graphics::Gdi::{DeleteObject, HFONT};
use windows::Win32::System::Com::CoTaskMemFree;
use windows::Win32::UI::Shell::Common::ITEMIDLIST;
use windows::Win32::UI::WindowsAndMessaging::{DestroyMenu, GetWindowTextLengthW, GetWindowTextW, HMENU, SendMessageW};
use windows::core::PCWSTR;

/// NUL-terminated UTF-16 copy of a string, ready to pass to Windows.
pub fn wide(s: impl AsRef<OsStr>) -> Vec<u16> {
    s.as_ref().encode_wide().chain(Some(0)).collect()
}

/// Reads UTF-16 text up to the first NUL. Lossless: any file name survives.
pub fn from_wide(buf: &[u16]) -> OsString {
    let n = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    OsString::from_wide(&buf[..n])
}

pub fn pcwstr(w: &[u16]) -> PCWSTR {
    PCWSTR(w.as_ptr())
}

/// Copies text into a buffer Windows gave us, truncating and always NUL-terminating.
///
/// # Safety
/// `buf` must point to at least `cap` writable u16s.
pub unsafe fn copy_to(buf: *mut u16, cap: i32, text: impl AsRef<OsStr>) {
    if buf.is_null() || cap <= 0 {
        return;
    }
    let mut n = 0;
    for c in text.as_ref().encode_wide().take(cap as usize - 1) {
        unsafe { *buf.add(n) = c };
        n += 1;
    }
    unsafe { *buf.add(n) = 0 };
}

/// SendMessage with plain numbers, for the many control messages that take them.
pub fn send(h: HWND, msg: u32, wp: usize, lp: isize) -> isize {
    unsafe { SendMessageW(h, msg, Some(WPARAM(wp)), Some(LPARAM(lp))).0 }
}

pub fn window_text(hwnd: HWND) -> OsString {
    unsafe {
        let len = GetWindowTextLengthW(hwnd);
        let mut buf = vec![0u16; len as usize + 1];
        let n = GetWindowTextW(hwnd, &mut buf);
        OsString::from_wide(&buf[..n as usize])
    }
}

pub fn scale(px: i32, dpi: u32) -> i32 {
    (px as i64 * dpi as i64 / 96) as i32
}

/// Owns a shell item ID list.
pub struct Pidl(pub *mut ITEMIDLIST);

impl Drop for Pidl {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { CoTaskMemFree(Some(self.0 as *const _)) };
        }
    }
}

pub struct OwnedMenu(pub HMENU);

impl Drop for OwnedMenu {
    fn drop(&mut self) {
        unsafe {
            let _ = DestroyMenu(self.0);
        }
    }
}

pub struct Font(pub HFONT);

impl Drop for Font {
    fn drop(&mut self) {
        unsafe {
            let _ = DeleteObject(self.0.into());
        }
    }
}
