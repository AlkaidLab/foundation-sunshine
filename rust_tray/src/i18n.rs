//! Internationalization (i18n) module for tray icon
//!
//! Supports Chinese, English, and Japanese translations.

use std::collections::HashMap;
use parking_lot::RwLock;
use once_cell::sync::Lazy;

/// Supported locales
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Locale {
    English,
    Chinese,
    Japanese,
}

impl Default for Locale {
    fn default() -> Self {
        Locale::English
    }
}

impl From<&str> for Locale {
    fn from(s: &str) -> Self {
        match s.to_lowercase().as_str() {
            "zh" | "zh_cn" | "zh_tw" | "chinese" => Locale::Chinese,
            "ja" | "ja_jp" | "japanese" => Locale::Japanese,
            _ => Locale::English,
        }
    }
}

/// String keys for localization
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum StringKey {
    // Menu items
    OpenSunshine,
    // VDD submenu
    VddBaseDisplay,
    VddCreate,
    VddClose,
    VddPersistent,
    VddPersistentConfirmTitle,
    VddPersistentConfirmMsg,
    // Advanced Settings submenu
    AdvancedSettings,
    CloseApp,
    CloseAppConfirmTitle,
    CloseAppConfirmMsg,
    // Language submenu
    Language,
    Chinese,
    English,
    Japanese,
    StarProject,
    // Visit Project submenu
    VisitProject,
    VisitProjectSunshine,
    VisitProjectMoonlight,
    ResetDisplayDeviceConfig,
    ResetDisplayConfirmTitle,
    ResetDisplayConfirmMsg,
    Restart,
    Quit,

    // Notifications
    StreamStarted,
    StreamingStartedFor,
    StreamPaused,
    StreamingPausedFor,
    ApplicationStopped,
    ApplicationStoppedMsg,
    IncomingPairingRequest,
    ClickToCompletePairing,

    // Dialog messages
    QuitTitle,
    QuitMessage,
    ErrorTitle,
}

/// Current locale storage
static CURRENT_LOCALE: Lazy<RwLock<Locale>> = Lazy::new(|| RwLock::new(Locale::English));

/// Translation tables
static TRANSLATIONS: Lazy<HashMap<(Locale, StringKey), &'static str>> = Lazy::new(|| {
    let mut m = HashMap::new();

    // English translations
    m.insert((Locale::English, StringKey::OpenSunshine), "Open GUI");
    // VDD submenu
    m.insert((Locale::English, StringKey::VddBaseDisplay), "Foundation Display");
    m.insert((Locale::English, StringKey::VddCreate), "Create Virtual Display");
    m.insert((Locale::English, StringKey::VddClose), "Close Virtual Display");
    m.insert((Locale::English, StringKey::VddPersistent), "Keep Enabled");
    m.insert((Locale::English, StringKey::VddPersistentConfirmTitle), "Keep Virtual Display Enabled");
    m.insert((Locale::English, StringKey::VddPersistentConfirmMsg), "By enabling this option, the virtual display will NOT be closed after you stop streaming.\n\nDo you want to enable this feature?");
    // Advanced Settings submenu
    m.insert((Locale::English, StringKey::AdvancedSettings), "Advanced Settings");
    m.insert((Locale::English, StringKey::CloseApp), "Clear Cache");
    m.insert((Locale::English, StringKey::CloseAppConfirmTitle), "Clear Cache");
    m.insert((Locale::English, StringKey::CloseAppConfirmMsg), "This operation will clear streaming state, may terminate the streaming application, and clean up related processes and state. Do you want to continue?");
    m.insert((Locale::English, StringKey::Language), "Language");
    m.insert((Locale::English, StringKey::Chinese), "中文 (Chinese)");
    m.insert((Locale::English, StringKey::English), "English");
    m.insert((Locale::English, StringKey::Japanese), "日本語 (Japanese)");
    m.insert((Locale::English, StringKey::StarProject), "Visit Website");
    m.insert((Locale::English, StringKey::VisitProject), "Visit Project");
    m.insert((Locale::English, StringKey::VisitProjectSunshine), "Sunshine");
    m.insert((Locale::English, StringKey::VisitProjectMoonlight), "Moonlight");
    m.insert((Locale::English, StringKey::ResetDisplayDeviceConfig), "Reset Display");
    m.insert((Locale::English, StringKey::ResetDisplayConfirmTitle), "Reset Display");
    m.insert((Locale::English, StringKey::ResetDisplayConfirmMsg), "Are you sure you want to reset display device memory? This action cannot be undone.");
    m.insert((Locale::English, StringKey::Restart), "Restart");
    m.insert((Locale::English, StringKey::Quit), "Quit");
    m.insert((Locale::English, StringKey::StreamStarted), "Stream Started");
    m.insert((Locale::English, StringKey::StreamingStartedFor), "Streaming started for %s");
    m.insert((Locale::English, StringKey::StreamPaused), "Stream Paused");
    m.insert((Locale::English, StringKey::StreamingPausedFor), "Streaming paused for %s");
    m.insert((Locale::English, StringKey::ApplicationStopped), "Application Stopped");
    m.insert((Locale::English, StringKey::ApplicationStoppedMsg), "Application %s successfully stopped");
    m.insert((Locale::English, StringKey::IncomingPairingRequest), "Incoming PIN Request From: %s");
    m.insert((Locale::English, StringKey::ClickToCompletePairing), "Click here to enter PIN");
    m.insert((Locale::English, StringKey::QuitTitle), "Wait! Don't Leave Me! T_T");
    m.insert((Locale::English, StringKey::QuitMessage), "Nooo! You can't just quit like that!\nAre you really REALLY sure you want to leave?\nI'll miss you... but okay, if you must...\n\n(This will also close the Sunshine GUI application.)");
    m.insert((Locale::English, StringKey::ErrorTitle), "Error");

    // Chinese translations
    m.insert((Locale::Chinese, StringKey::OpenSunshine), "打开基地面板");
    // VDD submenu
    m.insert((Locale::Chinese, StringKey::VddBaseDisplay), "基地显示器");
    m.insert((Locale::Chinese, StringKey::VddCreate), "创建显示器");
    m.insert((Locale::Chinese, StringKey::VddClose), "关闭显示器");
    m.insert((Locale::Chinese, StringKey::VddPersistent), "保持启用");
    m.insert((Locale::Chinese, StringKey::VddPersistentConfirmTitle), "保持开启虚拟显示器");
    m.insert((Locale::Chinese, StringKey::VddPersistentConfirmMsg), "启用此选项后，在串流结束后基地显示器将不会被自动关闭。\n\n确定要开启此功能吗？");
    // Advanced Settings submenu
    m.insert((Locale::Chinese, StringKey::AdvancedSettings), "高级设置");
    m.insert((Locale::Chinese, StringKey::CloseApp), "清理缓存");
    m.insert((Locale::Chinese, StringKey::CloseAppConfirmTitle), "清理缓存");
    m.insert((Locale::Chinese, StringKey::CloseAppConfirmMsg), "此操作将会清理串流状态，可能会终止串流应用，并清理相关进程和状态。是否继续？");
    m.insert((Locale::Chinese, StringKey::Language), "语言 Language");
    m.insert((Locale::Chinese, StringKey::Chinese), "中文");
    m.insert((Locale::Chinese, StringKey::English), "English (英语)");
    m.insert((Locale::Chinese, StringKey::Japanese), "日本語 (日语)");
    m.insert((Locale::Chinese, StringKey::StarProject), "访问官网");
    m.insert((Locale::Chinese, StringKey::VisitProject), "访问项目地址");
    m.insert((Locale::Chinese, StringKey::VisitProjectSunshine), "Sunshine");
    m.insert((Locale::Chinese, StringKey::VisitProjectMoonlight), "Moonlight");
    m.insert((Locale::Chinese, StringKey::ResetDisplayDeviceConfig), "重置显示器");
    m.insert((Locale::Chinese, StringKey::ResetDisplayConfirmTitle), "重置显示器");
    m.insert((Locale::Chinese, StringKey::ResetDisplayConfirmMsg), "确定要重置显示器设备记忆吗？此操作无法撤销。");
    m.insert((Locale::Chinese, StringKey::Restart), "重新启动");
    m.insert((Locale::Chinese, StringKey::Quit), "退出");
    m.insert((Locale::Chinese, StringKey::StreamStarted), "串流已开始");
    m.insert((Locale::Chinese, StringKey::StreamingStartedFor), "已开始串流：%s");
    m.insert((Locale::Chinese, StringKey::StreamPaused), "串流已暂停");
    m.insert((Locale::Chinese, StringKey::StreamingPausedFor), "已暂停串流：%s");
    m.insert((Locale::Chinese, StringKey::ApplicationStopped), "应用已停止");
    m.insert((Locale::Chinese, StringKey::ApplicationStoppedMsg), "应用 %s 已成功停止");
    m.insert((Locale::Chinese, StringKey::IncomingPairingRequest), "来自 %s 的PIN请求");
    m.insert((Locale::Chinese, StringKey::ClickToCompletePairing), "点击此处完成PIN验证");
    m.insert((Locale::Chinese, StringKey::QuitTitle), "真的要退出吗");
    m.insert((Locale::Chinese, StringKey::QuitMessage), "你不能退出!\n那么想退吗? 真拿你没办法呢, 继续点一下吧~\n\n这将同时关闭Sunshine GUI应用程序。");
    m.insert((Locale::Chinese, StringKey::ErrorTitle), "错误");

    // Japanese translations
    m.insert((Locale::Japanese, StringKey::OpenSunshine), "GUIを開く");
    // VDD submenu
    m.insert((Locale::Japanese, StringKey::VddBaseDisplay), "基地ディスプレイ");
    m.insert((Locale::Japanese, StringKey::VddCreate), "仮想ディスプレイを作成");
    m.insert((Locale::Japanese, StringKey::VddClose), "仮想ディスプレイを閉じる");
    m.insert((Locale::Japanese, StringKey::VddPersistent), "常駐仮想ディスプレイを");
    m.insert((Locale::Japanese, StringKey::VddPersistentConfirmTitle), "仮想ディスプレイを有効に保つ");
    m.insert((Locale::Japanese, StringKey::VddPersistentConfirmMsg), "このオプションを有効にすると、ストリーミング終了後に仮想ディスプレイは**自動的に閉じられません**。\n\nこの機能を有効にしますか？");
    // Advanced Settings submenu
    m.insert((Locale::Japanese, StringKey::AdvancedSettings), "詳細設定");
    m.insert((Locale::Japanese, StringKey::CloseApp), "キャッシュをクリア");
    m.insert((Locale::Japanese, StringKey::CloseAppConfirmTitle), "キャッシュをクリア");
    m.insert((Locale::Japanese, StringKey::CloseAppConfirmMsg), "この操作はストリーミング状態をクリアし、ストリーミングアプリケーションを終了する可能性があり、関連するプロセスと状態をクリーンアップします。続行しますか？");
    m.insert((Locale::Japanese, StringKey::Language), "言語 Language");
    m.insert((Locale::Japanese, StringKey::Chinese), "中文 (中国語)");
    m.insert((Locale::Japanese, StringKey::English), "English (英語)");
    m.insert((Locale::Japanese, StringKey::Japanese), "日本語");
    m.insert((Locale::Japanese, StringKey::StarProject), "公式サイトを訪問");
    m.insert((Locale::Japanese, StringKey::VisitProject), "プロジェクトアドレスを訪問");
    m.insert((Locale::Japanese, StringKey::VisitProjectSunshine), "Sunshine");
    m.insert((Locale::Japanese, StringKey::VisitProjectMoonlight), "Moonlight");
    m.insert((Locale::Japanese, StringKey::ResetDisplayDeviceConfig), "ディスプレイをリセット");
    m.insert((Locale::Japanese, StringKey::ResetDisplayConfirmTitle), "ディスプレイをリセット");
    m.insert((Locale::Japanese, StringKey::ResetDisplayConfirmMsg), "ディスプレイデバイスのメモリをリセットしてもよろしいですか？この操作は元に戻せません。");
    m.insert((Locale::Japanese, StringKey::Restart), "再起動");
    m.insert((Locale::Japanese, StringKey::Quit), "終了");
    m.insert((Locale::Japanese, StringKey::StreamStarted), "ストリーム開始");
    m.insert((Locale::Japanese, StringKey::StreamingStartedFor), "%s のストリーミングを開始しました");
    m.insert((Locale::Japanese, StringKey::StreamPaused), "ストリーム一時停止");
    m.insert((Locale::Japanese, StringKey::StreamingPausedFor), "%s のストリーミングを一時停止しました");
    m.insert((Locale::Japanese, StringKey::ApplicationStopped), "アプリケーション停止");
    m.insert((Locale::Japanese, StringKey::ApplicationStoppedMsg), "アプリケーション %s が正常に停止しました");
    m.insert((Locale::Japanese, StringKey::IncomingPairingRequest), "%s からのPIN要求");
    m.insert((Locale::Japanese, StringKey::ClickToCompletePairing), "クリックしてPIN認証を完了");
    m.insert((Locale::Japanese, StringKey::QuitTitle), "本当に終了しますか？");
    m.insert((Locale::Japanese, StringKey::QuitMessage), "終了できません！\n本当に終了したいですか？\n\nこれによりSunshine GUIアプリケーションも閉じられます。");
    m.insert((Locale::Japanese, StringKey::ErrorTitle), "エラー");

    m
});

/// Get current locale
pub fn get_locale() -> Locale {
    *CURRENT_LOCALE.read()
}

/// Set current locale
pub fn set_locale(locale: Locale) {
    *CURRENT_LOCALE.write() = locale;
}

/// Set locale from string
pub fn set_locale_str(locale_str: &str) {
    set_locale(Locale::from(locale_str));
}

/// Get localized string
pub fn get_string(key: StringKey) -> &'static str {
    let locale = get_locale();

    // Try current locale first
    if let Some(s) = TRANSLATIONS.get(&(locale, key)) {
        return s;
    }

    // Fallback to English
    if let Some(s) = TRANSLATIONS.get(&(Locale::English, key)) {
        return s;
    }

    // Return empty string if not found
    ""
}

/// Get localized string with format substitution
pub fn get_string_fmt(key: StringKey, arg: &str) -> String {
    let template = get_string(key);
    template.replace("%s", arg)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_locale_from_str() {
        assert_eq!(Locale::from("zh"), Locale::Chinese);
        assert_eq!(Locale::from("zh_CN"), Locale::Chinese);
        assert_eq!(Locale::from("ja"), Locale::Japanese);
        assert_eq!(Locale::from("en"), Locale::English);
        assert_eq!(Locale::from("unknown"), Locale::English);
    }

    #[test]
    fn test_get_string() {
        set_locale(Locale::English);
        assert_eq!(get_string(StringKey::OpenSunshine), "Open Sunshine");

        set_locale(Locale::Chinese);
        assert_eq!(get_string(StringKey::OpenSunshine), "打开 Sunshine");

        set_locale(Locale::Japanese);
        assert_eq!(get_string(StringKey::OpenSunshine), "Sunshineを開く");
    }

    #[test]
    fn test_get_string_fmt() {
        set_locale(Locale::English);
        assert_eq!(get_string_fmt(StringKey::StreamingStartedFor, "TestApp"), "Streaming started for TestApp");
    }
}
