//! Menu module - Menu building and state management
//!
//! This module handles the actual menu construction from menu_items definitions.
//! It converts the declarative MenuItemInfo into actual muda menu items.
//!
//! Note: muda menu items use Rc internally and are not thread-safe,
//! so we store MenuId strings and item IDs for cross-thread access.

use muda::{Menu, MenuItem, PredefinedMenuItem, Submenu, CheckMenuItem, MenuEvent, MenuId};
use std::collections::HashMap;
use parking_lot::RwLock;
use once_cell::sync::Lazy;

use crate::i18n::get_string;
use crate::menu_items::{self, ItemKind};

/// Registry entry - maps string item_id to muda MenuId string
pub struct MenuIdEntry {
    pub item_id: String,
    pub menu_id: String,
    pub kind: ItemKind,
}

// Thread-local storage for actual muda items (not thread-safe)
thread_local! {
    static CREATED_ITEMS: std::cell::RefCell<HashMap<String, CreatedItem>> =
        std::cell::RefCell::new(HashMap::new());
}

enum CreatedItem {
    Regular(MenuItem),
    Check(CheckMenuItem),
    Submenu(Submenu),
}

/// Global menu ID registry - maps item_id to muda MenuId string
/// This is thread-safe as it only stores strings
static MENU_ID_REGISTRY: Lazy<RwLock<HashMap<String, MenuIdEntry>>> =
    Lazy::new(|| RwLock::new(HashMap::new()));

/// Clear the registries
fn clear_registry() {
    MENU_ID_REGISTRY.write().clear();
    CREATED_ITEMS.with(|items| items.borrow_mut().clear());
}

/// Register a menu item
fn register_item(item_id: &str, menu_id: &MenuId, kind: ItemKind) {
    let menu_id_str = format!("{:?}", menu_id);
    MENU_ID_REGISTRY.write().insert(item_id.to_string(), MenuIdEntry {
        item_id: item_id.to_string(),
        menu_id: menu_id_str,
        kind,
    });
}

/// Store a created item for thread-local access
fn store_created_item(item_id: &str, item: CreatedItem) {
    CREATED_ITEMS.with(|items| {
        items.borrow_mut().insert(item_id.to_string(), item);
    });
}

/// Set a checkbox item's state by item_id
pub fn set_check_state_by_id(item_id: &str, checked: bool) {
    CREATED_ITEMS.with(|items| {
        if let Some(CreatedItem::Check(item)) = items.borrow().get(item_id) {
            item.set_checked(checked);
        }
    });
}

/// Get a checkbox item's current state by item_id
pub fn get_check_state_by_id(item_id: &str) -> Option<bool> {
    CREATED_ITEMS.with(|items| {
        items.borrow().get(item_id).and_then(|item| {
            if let CreatedItem::Check(check_item) = item {
                Some(check_item.is_checked())
            } else {
                None
            }
        })
    })
}

/// Set a menu item's enabled state by item_id
pub fn set_item_enabled_by_id(item_id: &str, enabled: bool) {
    CREATED_ITEMS.with(|items| {
        match items.borrow().get(item_id) {
            Some(CreatedItem::Regular(item)) => item.set_enabled(enabled),
            Some(CreatedItem::Check(item)) => item.set_enabled(enabled),
            Some(CreatedItem::Submenu(item)) => item.set_enabled(enabled),
            None => {}
        }
    });
}

/// Identify the item_id for a menu event
pub fn identify_item_id(event: &MenuEvent) -> Option<String> {
    let event_id_str = format!("{:?}", event.id);
    let registry = MENU_ID_REGISTRY.read();
    for (item_id, entry) in registry.iter() {
        if entry.menu_id == event_id_str {
            return Some(item_id.clone());
        }
    }
    None
}

/// Build the menu from menu_items definitions
pub fn rebuild_menu() -> Menu {
    // Clear old registry
    clear_registry();

    let items = menu_items::get_all_items();

    // First pass: create all items and submenus
    let mut submenus: HashMap<&str, Submenu> = HashMap::new();
    let mut regular_items: HashMap<&str, Box<dyn muda::IsMenuItem>> = HashMap::new();

    // Sort items by order
    let mut sorted_items: Vec<_> = items.iter().collect();
    sorted_items.sort_by_key(|item| item.order);

    // Create submenus first
    for info in &sorted_items {
        if info.kind == ItemKind::Submenu {
            if let Some(key) = info.label_key {
                let submenu = Submenu::new(get_string(key), true);
                register_item(info.id, submenu.id(), info.kind);
                store_created_item(info.id, CreatedItem::Submenu(submenu.clone()));
                submenus.insert(info.id, submenu);
            }
        }
    }

    // Create all other items
    for info in &sorted_items {
        match info.kind {
            ItemKind::Action => {
                if let Some(key) = info.label_key {
                    let item = MenuItem::new(get_string(key), true, None);
                    register_item(info.id, item.id(), info.kind);
                    store_created_item(info.id, CreatedItem::Regular(item.clone()));
                    regular_items.insert(info.id, Box::new(item));
                }
            }
            ItemKind::Check => {
                if let Some(key) = info.label_key {
                    let item = CheckMenuItem::new(get_string(key), true, info.default_checked, None);
                    register_item(info.id, item.id(), info.kind);
                    store_created_item(info.id, CreatedItem::Check(item.clone()));
                    regular_items.insert(info.id, Box::new(item));
                }
            }
            ItemKind::Separator => {
                let item = PredefinedMenuItem::separator();
                regular_items.insert(info.id, Box::new(item));
            }
            ItemKind::Submenu => {
                // Already handled above
            }
        }
    }

    // Second pass: add items to their parent submenus
    for info in &sorted_items {
        if let Some(parent_id) = info.parent {
            if let Some(submenu) = submenus.get(parent_id) {
                if info.kind == ItemKind::Submenu {
                    if let Some(child_submenu) = submenus.get(info.id) {
                        let _ = submenu.append(child_submenu);
                    }
                } else if let Some(item) = regular_items.remove(info.id) {
                    let _ = submenu.append(item.as_ref());
                }
            }
        }
    }

    // Third pass: build main menu with top-level items
    let menu = Menu::new();
    for info in &sorted_items {
        if info.parent.is_none() {
            if info.kind == ItemKind::Submenu {
                if let Some(submenu) = submenus.get(info.id) {
                    let _ = menu.append(submenu);
                }
            } else if let Some(item) = regular_items.remove(info.id) {
                let _ = menu.append(item.as_ref());
            }
        }
    }

    menu
}

// ============================================================================
// VDD Menu State Update
// ============================================================================

/// Update VDD menu item states
///
/// Called from C++ side to update menu item enabled/disabled/checked states.
///
/// # Parameters
/// * `can_create` - Whether "Create" item should be enabled
/// * `can_close` - Whether "Close" item should be enabled
/// * `is_persistent` - Whether "Keep Enabled" is checked
/// * `is_active` - Whether VDD is currently active (for checked states)
pub fn update_vdd_menu_state(can_create: bool, can_close: bool, is_persistent: bool, is_active: bool) {
    use menu_items::ids;

    // Update Create item
    // Checked when VDD is active, enabled based on can_create
    set_check_state_by_id(ids::VDD_CREATE, is_active);
    set_item_enabled_by_id(ids::VDD_CREATE, can_create);

    // Update Close item
    // Checked when VDD is NOT active, enabled based on can_close
    set_check_state_by_id(ids::VDD_CLOSE, !is_active);
    set_item_enabled_by_id(ids::VDD_CLOSE, can_close);

    // Update Keep Enabled item
    set_check_state_by_id(ids::VDD_PERSISTENT, is_persistent);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_get_all_items() {
        let items = menu_items::get_all_items();
        assert!(!items.is_empty());
    }
}
