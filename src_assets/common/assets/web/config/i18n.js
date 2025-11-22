import {createI18n} from "vue-i18n";

// Import only the fallback language files
import en from '../public/assets/locale/en.json'

export default async function() {
    // 先尝试从 /api/config 获取实时配置（会读取配置文件）
    let locale = "en";
    try {
        let config = await (await fetch("/api/config")).json();
        locale = config.locale ?? "en";
    } catch (e) {
        // 如果失败，回退到 /api/configLocale（从内存读取）
        try {
            let r = await (await fetch("/api/configLocale")).json();
            locale = r.locale ?? "en";
        } catch (e2) {
            console.error("Failed to get locale config", e, e2);
        }
    }
    
    document.querySelector('html').setAttribute('lang', locale);
    let messages = {
        en
    };
    try {
        if (locale !== 'en') {
            let r = await (await fetch(`/assets/locale/${locale}.json`)).json();
            messages[locale] = r;
        }
    } catch (e) {
        console.error("Failed to download translations", e);
    }
    const i18n = createI18n({
        legacy: false, // 使用 Composition API 模式
        locale: locale, // set locale
        fallbackLocale: 'en', // set fallback locale
        messages: messages,
        globalInjection: true, // 允许在模板中使用 $t
    })
    return i18n;
}
