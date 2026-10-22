//! Small helpers: UTF-16 strings for Windows, and owners that free Windows resources when dropped.

use std::ffi::{OsStr, OsString};
use std::os::windows::ffi::{OsStrExt, OsStringExt};

use windows::Win32::Foundation::{HWND, LPARAM, POINT, RECT, WPARAM};
use windows::Win32::Graphics::Gdi::{DeleteObject, HFONT};
use windows::Win32::System::Com::CoTaskMemFree;
use windows::Win32::System::WindowsProgramming::MulDiv;
use windows::Win32::UI::Input::KeyboardAndMouse::{GetKeyState, SetFocus, VIRTUAL_KEY};
use windows::Win32::UI::Shell::Common::ITEMIDLIST;
use windows::Win32::UI::WindowsAndMessaging::{
    DestroyMenu, GetClientRect, GetCursorPos, GetWindowRect, GetWindowTextLengthW, GetWindowTextW, HMENU, SendMessageW,
    SetWindowTextW,
};
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

pub fn set_text(hwnd: HWND, text: impl AsRef<OsStr>) {
    unsafe {
        let _ = SetWindowTextW(hwnd, pcwstr(&wide(text)));
    }
}

pub fn focus(hwnd: HWND) {
    unsafe {
        let _ = SetFocus(Some(hwnd));
    }
}

pub fn key_down(vk: VIRTUAL_KEY) -> bool {
    unsafe { GetKeyState(vk.0 as i32) < 0 }
}

/// A window's inside, from 0, 0.
pub fn client_rect(hwnd: HWND) -> RECT {
    let mut r = RECT::default();
    unsafe {
        let _ = GetClientRect(hwnd, &mut r);
    }
    r
}

/// A window's place on the screen.
pub fn window_rect(hwnd: HWND) -> RECT {
    let mut r = RECT::default();
    unsafe {
        let _ = GetWindowRect(hwnd, &mut r);
    }
    r
}

/// Where the mouse is, on the screen.
pub fn cursor_pos() -> POINT {
    let mut p = POINT::default();
    unsafe {
        let _ = GetCursorPos(&mut p);
    }
    p
}

/// A point as mouse messages carry it.
pub fn packed(x: i32, y: i32) -> isize {
    x as u16 as isize | ((y as u16 as isize) << 16)
}

/// a * b / c, rounded to the nearest whole number: Windows' MulDiv, which the C++ version's sizes go through.
pub fn mul_div(a: i32, b: i32, c: i32) -> i32 {
    unsafe { MulDiv(a, b, c) }
}

pub fn scale(px: i32, dpi: u32) -> i32 {
    mul_div(px, dpi as i32, 96)
}

pub fn unscale(px: i32, dpi: u32) -> i32 {
    mul_div(px, 96, dpi as i32)
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scale_rounds_as_the_cpp_version() {
        assert_eq!(scale(10, 120), 13); // 12.5 at 125%: a half goes up
        assert_eq!(scale(150, 168), 263); // 262.5 at 175%
        assert_eq!(unscale(299, 168), 171); // 170.86
        // A width saved and read back at 175% loses a pixel once at most, not a little more each time.
        let mut width = 300;
        for _ in 0..5 {
            width = scale(unscale(width, 168), 168);
        }
        assert_eq!(width, 299);
    }
}
