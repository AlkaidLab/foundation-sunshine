//! Menu Items Module - Centralized menu item definitions and handlers
//!
//! This is the ONLY file you need to modify when adding new menu items.
//!
//! To add a new menu item:
//! 1. Add a new entry to `define_menu_items!` macro
//! 2. Add translation strings in i18n.rs (StringKey enum and TRANSLATIONS)
//! 3. Done! No need to modify any other files.

use crate::i18n::{StringKey, get_string, set_locale_str};
use crate::actions::trigger_action;
use crate::dialogs;

/// Menu item handler function type
/// Returns true if action should proceed, false to cancel
pub type MenuHandler = fn() -> bool;

/// Menu item type
#[derive(Clone, Copy, PartialEq)]
pub enum ItemKind {
    /// Regular clickable item
    Action,
    /// Checkbox item
    Check,
    /// Separator line
    Separator,
    /// Submenu container (no direct action)
    Submenu,
}

/// Menu item definition
#[derive(Clone)]
pub struct MenuItemInfo {
    /// Unique identifier for this item
    pub id: &'static str,
    /// String key for localization
    pub label_key: Option<StringKey>,
    /// Item type
    pub kind: ItemKind,
    /// Parent submenu ID (None for top-level items)
    pub parent: Option<&'static str>,
    /// Handler function (for action items)
    pub handler: Option<MenuHandler>,
    /// Whether to rebuild menu after this action (e.g., language change)
    pub rebuild_menu: bool,
    /// Default checked state (for checkbox items)
    pub default_checked: bool,
    /// Sort order (lower = higher in menu)
    pub order: u32,
}

impl MenuItemInfo {
    /// Create an action item
    pub const fn action(id: &'static str, label_key: StringKey, parent: Option<&'static str>, order: u32) -> Self {
        Self {
            id,
            label_key: Some(label_key),
            kind: ItemKind::Action,
            parent,
            handler: None,
            rebuild_menu: false,
            default_checked: false,
            order,
        }
    }

    /// Create a checkbox item
    pub const fn check(id: &'static str, label_key: StringKey, parent: Option<&'static str>, default: bool, order: u32) -> Self {
        Self {
            id,
            label_key: Some(label_key),
            kind: ItemKind::Check,
            parent,
            handler: None,
            rebuild_menu: false,
            default_checked: default,
            order,
        }
    }

    /// Create a submenu
    pub const fn submenu(id: &'static str, label_key: StringKey, parent: Option<&'static str>, order: u32) -> Self {
        Self {
            id,
            label_key: Some(label_key),
            kind: ItemKind::Submenu,
            parent,
            handler: None,
            rebuild_menu: false,
            default_checked: false,
            order,
        }
    }

    /// Create a separator
    pub const fn separator(id: &'static str, parent: Option<&'static str>, order: u32) -> Self {
        Self {
            id,
            label_key: None,
            kind: ItemKind::Separator,
            parent,
            handler: None,
            rebuild_menu: false,
            default_checked: false,
            order,
        }
    }

    /// Set handler and return self (builder pattern)
    pub const fn with_handler(mut self, handler: MenuHandler) -> Self {
        self.handler = Some(handler);
        self
    }

    /// Mark this item as requiring menu rebuild after action
    pub const fn with_rebuild(mut self) -> Self {
        self.rebuild_menu = true;
        self
    }
}

// ============================================================================
// Menu Item IDs - Used for registration and lookup
// ============================================================================

pub mod ids {
    // Top-level items
    pub const OPEN_SUNSHINE: &str = "open_sunshine";
    pub const SEP_1: &str = "sep_1";
    pub const SEP_2: &str = "sep_2";
    pub const SEP_3: &str = "sep_3";
    pub const SEP_4: &str = "sep_4";
    pub const STAR_PROJECT: &str = "star_project";
    pub const RESTART: &str = "restart";
    pub const QUIT: &str = "quit";

    // VDD submenu
    pub const VDD_SUBMENU: &str = "vdd_submenu";
    pub const VDD_CREATE: &str = "vdd_create";
    pub const VDD_CLOSE: &str = "vdd_close";
    pub const VDD_PERSISTENT: &str = "vdd_persistent";

    // Advanced Settings submenu
    pub const ADVANCED_SUBMENU: &str = "advanced_submenu";
    pub const CLOSE_APP: &str = "close_app";
    pub const RESET_DISPLAY: &str = "reset_display";

    // Language submenu
    pub const LANGUAGE_SUBMENU: &str = "language_submenu";
    pub const LANG_CHINESE: &str = "lang_chinese";
    pub const LANG_ENGLISH: &str = "lang_english";
    pub const LANG_JAPANESE: &str = "lang_japanese";

    // Visit Project submenu
    pub const VISIT_SUBMENU: &str = "visit_submenu";
    pub const VISIT_SUNSHINE: &str = "visit_sunshine";
    pub const VISIT_MOONLIGHT: &str = "visit_moonlight";
}

// ============================================================================
// Handler Functions - The actual logic for each menu item
// ============================================================================

mod handlers {
    use super::*;

    /// 关闭应用前显示确认对话框
    pub fn close_app() -> bool {
        dialogs::show_confirm_dialog(
            get_string(StringKey::CloseAppConfirmTitle),
            get_string(StringKey::CloseAppConfirmMsg),
        )
    }

    /// 重置显示配置前显示确认对话框
    pub fn reset_display() -> bool {
        dialogs::show_confirm_dialog(
            get_string(StringKey::ResetDisplayConfirmTitle),
            get_string(StringKey::ResetDisplayConfirmMsg),
        )
    }

    /// 切换语言（只更新 UI，配置保存由 C++ 处理）
    pub fn lang_chinese() -> bool {
        set_locale_str("zh");
        true
    }

    pub fn lang_english() -> bool {
        set_locale_str("en");
        true
    }

    pub fn lang_japanese() -> bool {
        set_locale_str("ja");
        true
    }

    /// 退出前显示确认对话框
    pub fn quit() -> bool {
        dialogs::show_confirm_dialog(
            get_string(StringKey::QuitTitle),
            get_string(StringKey::QuitMessage),
        )
    }
}

// ============================================================================
// Menu Item Definitions - THE SINGLE SOURCE OF TRUTH
// ============================================================================

/// Get all menu item definitions
/// This is the ONLY place that defines the menu structure
pub fn get_all_items() -> Vec<MenuItemInfo> {
    use ids::*;

    vec![
        // ====== Top Level Items ======
        MenuItemInfo::action(OPEN_SUNSHINE, StringKey::OpenSunshine, None, 100),

        MenuItemInfo::separator(SEP_1, None, 200),

        // ====== VDD Submenu ======
        MenuItemInfo::submenu(VDD_SUBMENU, StringKey::VddBaseDisplay, None, 300),
        MenuItemInfo::check(VDD_CREATE, StringKey::VddCreate, Some(VDD_SUBMENU), false, 310),
        MenuItemInfo::check(VDD_CLOSE, StringKey::VddClose, Some(VDD_SUBMENU), false, 320),
        MenuItemInfo::check(VDD_PERSISTENT, StringKey::VddPersistent, Some(VDD_SUBMENU), false, 330),

        // ====== Advanced Settings Submenu ======
        MenuItemInfo::submenu(ADVANCED_SUBMENU, StringKey::AdvancedSettings, None, 400),
        MenuItemInfo::action(CLOSE_APP, StringKey::CloseApp, Some(ADVANCED_SUBMENU), 450)
            .with_handler(handlers::close_app),
        MenuItemInfo::action(RESET_DISPLAY, StringKey::ResetDisplayDeviceConfig, Some(ADVANCED_SUBMENU), 460)
            .with_handler(handlers::reset_display),

        MenuItemInfo::separator(SEP_2, None, 500),

        // ====== Language Submenu ======
        MenuItemInfo::submenu(LANGUAGE_SUBMENU, StringKey::Language, None, 600),
        MenuItemInfo::action(LANG_CHINESE, StringKey::Chinese, Some(LANGUAGE_SUBMENU), 610)
            .with_handler(handlers::lang_chinese)
            .with_rebuild(),
        MenuItemInfo::action(LANG_ENGLISH, StringKey::English, Some(LANGUAGE_SUBMENU), 620)
            .with_handler(handlers::lang_english)
            .with_rebuild(),
        MenuItemInfo::action(LANG_JAPANESE, StringKey::Japanese, Some(LANGUAGE_SUBMENU), 630)
            .with_handler(handlers::lang_japanese)
            .with_rebuild(),

        MenuItemInfo::separator(SEP_3, None, 700),

        // ====== Star Project ======
        MenuItemInfo::action(STAR_PROJECT, StringKey::StarProject, None, 800),

        // ====== Visit Project Submenu ======
        MenuItemInfo::submenu(VISIT_SUBMENU, StringKey::VisitProject, None, 900),
        MenuItemInfo::action(VISIT_SUNSHINE, StringKey::VisitProjectSunshine, Some(VISIT_SUBMENU), 910),
        MenuItemInfo::action(VISIT_MOONLIGHT, StringKey::VisitProjectMoonlight, Some(VISIT_SUBMENU), 920),

        MenuItemInfo::separator(SEP_4, None, 1000),

        // ====== Restart & Quit ======
        MenuItemInfo::action(RESTART, StringKey::Restart, None, 1100),
        MenuItemInfo::action(QUIT, StringKey::Quit, None, 1200)
            .with_handler(handlers::quit),
    ]
}

/// Execute the handler for a menu item by ID
/// Returns (handled_locally, needs_rebuild)
pub fn execute_handler(item_id: &str) -> (bool, bool) {
    let items = get_all_items();

    if let Some(item) = items.iter().find(|i| i.id == item_id) {
        let needs_rebuild = item.rebuild_menu;

        // Execute Rust handler if present (for dialogs, language changes, etc.)
        // Handler returns false to cancel the action (e.g., user clicked "No" on dialog)
        if let Some(handler) = item.handler {
            if !handler() {
                return (true, false);  // Handled but cancelled, no rebuild
            }
        }

        // Trigger C++ callback for action items
        trigger_action_for_id(item_id);
        return (true, needs_rebuild);
    }

    (false, false)
}

/// Trigger C++ callback with action ID
fn trigger_action_for_id(item_id: &str) {
    trigger_action(item_id);
}
