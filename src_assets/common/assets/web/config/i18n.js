import {createI18n} from "vue-i18n";
import { getBootstrapConfig, getBootstrapLocale } from './bootstrapData.js'

// Import only the fallback language files
import en from '../public/assets/locale/en.json'

// 与后端 string_restricted_f("locale", ...) 白名单保持一致
const SUPPORTED_LOCALES = [
    'bg', 'cs', 'de', 'en', 'en_GB', 'en_US', 'es', 'fr',
    'it', 'ja', 'pt', 'ru', 'sv', 'tr', 'zh', 'zh_TW',
];

// 把浏览器 / 系统语言（navigator.language，例如 "zh-CN" / "en-US"）映射到支持的 locale
export function detectSystemLocale() {
    try {
        const candidates = [];
        if (typeof navigator !== 'undefined') {
            if (Array.isArray(navigator.languages)) candidates.push(...navigator.languages);
            if (navigator.language) candidates.push(navigator.language);
        }
        for (const raw of candidates) {
            if (!raw) continue;
            const norm = raw.replace('-', '_'); // zh-CN -> zh_CN
            // 1) 完整匹配（en_GB / en_US / zh_TW）
            if (SUPPORTED_LOCALES.includes(norm)) return norm;
            // 2) 繁体中文 / 港澳粤 → zh_TW
            const lower = norm.toLowerCase();
            if (lower.startsWith('zh') && /(_tw|_hk|_mo|hant)/.test(lower)) return 'zh_TW';
            // 3) 主语言匹配（zh_CN -> zh，en_AU -> en，...）
            const primary = norm.split('_')[0];
            if (SUPPORTED_LOCALES.includes(primary)) return primary;
        }
    } catch (e) {
        console.warn('detectSystemLocale failed', e);
    }
    return 'en';
}

export default async function() {
    // 优先从 /api/config 拿；读不到再试 /api/configLocale（包括 /api/config 返回但 locale 为空的情况）
    // 全都拿不到才走系统语言探测，最后回退 en
    let locale = null;
    try {
        const config = await getBootstrapConfig();
        locale = config.locale || null;
    } catch (e) {
        console.warn("Failed to get /api/config", e);
    }
    if (!locale) {
        try {
            const configLocale = await getBootstrapLocale();
            locale = configLocale.locale || null;
        } catch (e2) {
            console.error("Failed to get /api/configLocale", e2);
        }
    }
    if (!locale) {
        locale = detectSystemLocale();
    }
    
    // html[lang] 用 BCP-47 连字符形式，让 辅助技术 / 翻译插件 能正确识别 en_US -> en-US, zh_TW -> zh-TW
    document.querySelector('html').setAttribute('lang', locale.replace(/_/g, '-'));
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
        warnHtmlMessage: false, // 禁用 HTML 消息警告（因为我们使用 v-html 来渲染受信任的翻译内容）
    })
    return i18n;
}
