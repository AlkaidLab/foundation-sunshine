//! Menu actions module
//! 
//! Provides callback mechanism for menu actions.
//! C++ side registers a callback that receives action ID strings.

use parking_lot::RwLock;
use once_cell::sync::Lazy;
use std::ffi::CString;

/// Callback function type for menu actions (receives null-terminated C string)
pub type ActionCallback = extern "C" fn(action_id: *const std::ffi::c_char);

/// Global callback storage
static ACTION_CALLBACK: Lazy<RwLock<Option<ActionCallback>>> = Lazy::new(|| RwLock::new(None));

/// Register the callback for menu actions
pub fn register_callback(callback: ActionCallback) {
    *ACTION_CALLBACK.write() = Some(callback);
}

/// Trigger a menu action by string ID
pub fn trigger_action(action_id: &str) {
    if let Some(callback) = *ACTION_CALLBACK.read() {
        if let Ok(c_str) = CString::new(action_id) {
            callback(c_str.as_ptr());
        }
    }
}
