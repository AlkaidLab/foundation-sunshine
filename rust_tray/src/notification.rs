//! Windows Toast Notification module
//!
//! Provides native Windows toast notifications using WinRT.

use winrt_notification::{Duration, Sound, Toast};

/// Application ID for toast notifications
///
/// Using "Sunshine" as a simple app identifier. For full Windows integration
/// (notification center grouping, settings, etc.), this should ideally match
/// a shortcut in the Start Menu with the same AppUserModelID property.
const APP_ID: &str = "Sunshine";

/// Notification icon type
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NotificationIcon {
    Normal = 0,
    Playing = 1,
    Pausing = 2,
    Locked = 3,
}

impl From<i32> for NotificationIcon {
    fn from(value: i32) -> Self {
        match value {
            1 => NotificationIcon::Playing,
            2 => NotificationIcon::Pausing,
            3 => NotificationIcon::Locked,
            _ => NotificationIcon::Normal,
        }
    }
}

/// Show a Windows toast notification
///
/// # Arguments
/// * `title` - The notification title
/// * `body` - The notification body text
/// * `icon_type` - The icon type to use
///
/// # Returns
/// `true` if the notification was shown successfully, `false` otherwise
pub fn show_notification(title: &str, body: &str, _icon_type: NotificationIcon) -> bool {
    // Use winrt-notification to show a toast
    let result = Toast::new(APP_ID)
        .title(title)
        .text1(body)
        .duration(Duration::Short)
        .sound(Some(Sound::Default))
        .show();

    match result {
        Ok(_) => true,
        Err(e) => {
            eprintln!("Failed to show notification: {:?}", e);
            false
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_notification_icon_from() {
        assert_eq!(NotificationIcon::from(0), NotificationIcon::Normal);
        assert_eq!(NotificationIcon::from(1), NotificationIcon::Playing);
        assert_eq!(NotificationIcon::from(2), NotificationIcon::Pausing);
        assert_eq!(NotificationIcon::from(3), NotificationIcon::Locked);
        assert_eq!(NotificationIcon::from(99), NotificationIcon::Normal);
    }
}
