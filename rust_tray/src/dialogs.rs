//! Dialog utilities module (Windows only)
//!
//! 提供系统对话框功能。

use std::ffi::OsStr;
use std::os::windows::ffi::OsStrExt;

/// 显示确认对话框
pub fn show_confirm_dialog(title: &str, message: &str) -> bool {
    use windows_sys::Win32::UI::WindowsAndMessaging::{MessageBoxW, MB_YESNO, MB_ICONQUESTION, IDYES};

    let wide_title: Vec<u16> = OsStr::new(title)
        .encode_wide()
        .chain(std::iter::once(0))
        .collect();

    let wide_message: Vec<u16> = OsStr::new(message)
        .encode_wide()
        .chain(std::iter::once(0))
        .collect();

    unsafe {
        let result = MessageBoxW(
            std::ptr::null_mut(),
            wide_message.as_ptr(),
            wide_title.as_ptr(),
            MB_YESNO | MB_ICONQUESTION,
        );
        result == IDYES
    }
}
