import {createI18n} from "vue-i18n";
import { ref } from 'vue'
import { LOCALE_SYSTEM, choiceOf, readLocalePref, watchSystemLanguage, writeLocalePref } from 'foundation-ui/locale/localePref.js'
import { getBootstrapLocale } from './bootstrapData.js'
import { hostData } from '../composables/hostData.js'

// Import only the fallback language files
import en from '../public/assets/locale/en.json'

// 与后端 string_restricted_f("locale", ...) 白名单保持一致
const SUPPORTED_LOCALES = [
    'bg', 'cs', 'de', 'en', 'en_GB', 'en_US', 'es', 'fr',
    'it', 'ja', 'ko', 'pl', 'pt', 'pt_BR', 'ru', 'sv', 'tr', 'uk', 'zh', 'zh_TW',
];

// The names of the languages the host offers (the picker of the first screen of the guide)
export const LOCALE_NAMES = Object.freeze({
    bg: 'Български', cs: 'Čeština', de: 'Deutsch', en: 'English', en_GB: 'English (UK)', en_US: 'English (US)', es: 'Español', fr: 'Français',
    it: 'Italiano', ja: '日本語', ko: '한국어', pl: 'Polski', pt: 'Português', pt_BR: 'Português (Brasil)', ru: 'Русский', sv: 'Svenska', tr: 'Türkçe', uk: 'Українська', zh: '简体中文', zh_TW: '繁體中文',
});
export const SUPPORTED = SUPPORTED_LOCALES;

// What the person chose: 跟随系统 (the default, stored as `system`) or a language; and the language that is shown. The language of the operating system is followed live.
export const localeChoice = ref(LOCALE_SYSTEM);
export const activeLocale = ref('en');
let webI18n = null;

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
    // Cache-first (a page of the panel is as smooth as a native app): the language of the host as it was last time is known at once, so the first render does not wait
    // for the host (every page of the Web UI is a document of its own: it waited for /api/config here, on every page change). The fresh answer is read in the background and switches
    // the language only when the host's has changed and the person has not chosen one.
    let revalidate = null;
    const kept = hostData.peek('config');
    if (kept && kept.locale) {
        locale = kept.locale;
        revalidate = hostData.refresh('config').then((fresh) => fresh?.locale || null).catch(() => null);
    } else {
        try {
            const config = await hostData.refresh('config');
            locale = config.locale || null;
        } catch (e) {
            console.warn("Failed to get /api/config", e);
        }
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
    // the choice of the person (stored by this window): 跟随系统 gives the language of the system, a code is that language; without one the host's language stands
    const hostLocale = locale;
    const pref = readLocalePref();
    if (pref === LOCALE_SYSTEM) locale = detectSystemLocale();
    else if (pref && SUPPORTED_LOCALES.includes(pref)) locale = pref;
    localeChoice.value = choiceOf(pref, hostLocale, detectSystemLocale());
    activeLocale.value = locale;
    
    // html[lang] 用 BCP-47 连字符形式，让 辅助技术 / 翻译插件 能正确识别 en_US -> en-US, zh_TW -> zh-TW
    document.querySelector('html').setAttribute('lang', locale.replace(/_/g, '-'));
    let messages = {
        en
    };
    try {
        if (locale === 'zh_TW') messages.zh = await (await fetch('/assets/locale/zh.json')).json();
        if (locale !== 'en') messages[locale] = await (await fetch('/assets/locale/' + locale + '.json')).json();
    } catch (e) {
        console.error("Failed to download translations", e);
    }
    const i18n = createI18n({
        legacy: false, // 使用 Composition API 模式
        locale: locale, // set locale
        fallbackLocale: { zh_TW: ['zh', 'en'], default: ['en'] },
        messages: messages,
        globalInjection: true, // 允许在模板中使用 $t
        warnHtmlMessage: false, // 禁用 HTML 消息警告（因为我们使用 v-html 来渲染受信任的翻译内容）
    })
    webI18n = i18n;
    if (revalidate) revalidate.then((fresh) => { if (fresh && fresh !== hostLocale && !pref) applyLocale(fresh); });
    return i18n;
}

/** The code a choice shows: the system's language for `system`, else the code (when the host offers it). */
export const resolveChoice = (choice) => (choice === LOCALE_SYSTEM || !SUPPORTED_LOCALES.includes(choice) ? detectSystemLocale() : choice);

/** Shows a language at once: its messages are fetched when they are not loaded, the page's lang follows. */
export async function applyLocale(locale) {
    activeLocale.value = locale;
    if (!webI18n) return;
    try {
        if (locale === 'zh_TW' && !webI18n.global.availableLocales.includes('zh')) {
            webI18n.global.setLocaleMessage('zh', await (await fetch('/assets/locale/zh.json')).json());
        }
        if (locale !== 'en' && !webI18n.global.availableLocales.includes(locale)) {
            webI18n.global.setLocaleMessage(locale, await (await fetch('/assets/locale/' + locale + '.json')).json());
        }
    } catch (e) {
        console.error("Failed to download translations", e);
    }
    webI18n.global.locale.value = locale;
    document.querySelector('html').setAttribute('lang', locale.replace(/_/g, '-'));
}

/** The choice of the language, applied at once and stored as made ('system' is stored, not the code it gives). Returns the code that is shown. */
export async function setLocaleChoice(choice) {
    const value = choice === LOCALE_SYSTEM || SUPPORTED_LOCALES.includes(choice) ? choice : LOCALE_SYSTEM;
    writeLocalePref(value);
    localeChoice.value = value;
    const code = resolveChoice(value);
    await applyLocale(code);
    return code;
}

// the operating system changes its language: a window that follows it changes with it
watchSystemLanguage(() => { if (localeChoice.value === LOCALE_SYSTEM) void applyLocale(detectSystemLocale()); });
