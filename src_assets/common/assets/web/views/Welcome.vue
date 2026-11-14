<template>
  <div class="m-app m-root welcome-page">
    <MGuideFlow
      :steps="steps"
      :state="flowState"
      :callbacks="callbacks"
      :locale="selectedLocale"
      :texts="accountTexts"
      :storage-key="GUIDE_STORAGE_KEY"
      :qr="qrMatrix"
      :closable="false"
      :required="['account']"
    >
    </MGuideFlow>
  </div>
</template>

<script setup>
import { computed, ref, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import QRCode from 'qrcode'
import { MGuideFlow, buildGuideSteps, LOCALE_SYSTEM, choiceOf, readLocalePref, resolveSystemLocale, watchSystemLanguage, writeLocalePref } from 'foundation-ui/vue'
import { GUIDE_STORAGE_KEY, createAccountCallback, useGuide } from '../composables/useGuide.js'
import { getBootstrapLocale } from '../config/bootstrapData.js'
import { loadAutoTheme } from '../utils/theme.js'

// The first run starts here, with the host's own credential form as the first step of the guide. The host serves this page while it has
// no account; afterwards the sign-in leads to the Web UI, where the guide goes on with the next step.
const { t, locale, setLocaleMessage } = useI18n()
const selectedLocale = ref('en')

// Nothing is asked about what is sent: the first screen tells once (the notice of the shared guide) and 关于 › 隐私 holds the switches
const steps = buildGuideSteps({ skip: ['update'] })
const { state, callbacks: hostCallbacks } = useGuide()
state.account.exists = false
// The language of the first screen: 跟随系统 (the default, stored as such) or a language; shown at once
const localeChoice = ref(LOCALE_SYSTEM)
const systemLocale = () => resolveSystemLocale(navigator.languages?.length ? navigator.languages : [navigator.language], LOCALES.map((o) => o.value))
const resolved = ref('en') // set once the list of languages exists (onMounted)
const flowState = computed(() => ({
  ...state,
  locale: { choice: localeChoice.value, resolved: resolved.value, languages: LOCALES.map((o) => ({ code: o.value, name: o.label })) },
}))
function setLocale(choice) {
  localeChoice.value = choice
  writeLocalePref(choice)
  selectedLocale.value = choice === LOCALE_SYSTEM ? systemLocale() : choice
  changeLanguage()
}
const callbacks = { ...hostCallbacks, createAccount: createAccountCallback(t), setLocale }
const qrMatrix = (text) => QRCode.create(text, { errorCorrectionLevel: 'M' }).modules

// The credential form keeps the texts of the welcome page, in every language it has
const accountTexts = computed(() => ({
  // Chinese and English say it is the Web UI account (the shared texts); the other languages keep the welcome page's own words
  ...(/^(zh|en)/i.test(selectedLocale.value) ? {} : { titles: { account: t('welcome.greeting') }, leads: { account: t('welcome.create_creds') } }),
  account: {
    username: t('welcome.username'),
    password: t('welcome.password'),
    confirm: t('welcome.confirm_password'),
    mismatch: t('welcome.password_mismatch'),
    match: t('welcome.password_match'),
    ...(/^(zh|en)/i.test(selectedLocale.value) ? {} : { create: t('welcome.login'), note: t('welcome.create_creds_alert'), local: t('welcome.creds_local_only') }),
    done: `${t('welcome.success')} ${t('welcome.welcome_success')}`,
  },
}))

const LOCALES = Object.freeze([
  { value: 'en', label: 'English' }, { value: 'en_GB', label: 'English (UK)' }, { value: 'en_US', label: 'English (US)' },
  { value: 'zh', label: '简体中文' }, { value: 'zh_TW', label: '繁體中文' }, { value: 'de', label: 'Deutsch' },
  { value: 'fr', label: 'Français' }, { value: 'es', label: 'Español' }, { value: 'it', label: 'Italiano' },
  { value: 'ja', label: '日本語' }, { value: 'ko', label: '한국어' }, { value: 'ru', label: 'Русский' },
  { value: 'uk', label: 'Українська' }, { value: 'pt', label: 'Português' }, { value: 'pt_BR', label: 'Português (Brasil)' },
  { value: 'pl', label: 'Polski' }, { value: 'sv', label: 'Svenska' }, { value: 'tr', label: 'Türkçe' },
  { value: 'cs', label: 'Čeština' }, { value: 'bg', label: 'Български' },
])

loadAutoTheme()

// 加载语言（使用静态嵌入的翻译数据）
const loadLanguage = (lang) => {
  const welcomeLocales = window.__WELCOME_LOCALES__

  if (welcomeLocales && welcomeLocales[lang]) {
    // 使用嵌入的翻译数据设置到 i18n
    setLocaleMessage(lang, welcomeLocales[lang])
    locale.value = lang
    document.querySelector('html').setAttribute('lang', lang)
  } else {
    // 如果没有找到，使用英文
    locale.value = 'en'
    document.querySelector('html').setAttribute('lang', 'en')
  }
}

// 加载当前语言设置
onMounted(async () => {
  resolved.value = systemLocale()
  try {
    // 使用 /api/configLocale，这个 API 不需要认证（在 welcome 页面时可能还没有账号）
    const config = await getBootstrapLocale()
    // the stored choice first (跟随系统 gives the system's language); without one the host's language stands, and a window that has none follows the system
    const pref = readLocalePref()
    localeChoice.value = choiceOf(pref, config.locale, systemLocale())
    const code = pref === LOCALE_SYSTEM || (!pref && !config.locale) ? systemLocale() : (pref || config.locale)
    selectedLocale.value = code
    loadLanguage(code)
  } catch (e) {
    console.error('Failed to load locale config', e)
    loadLanguage('en')
  }
})

// 切换语言
const changeLanguage = () => {
  loadLanguage(selectedLocale.value)
}
// the operating system changes its language: a window that follows it changes with it
watchSystemLanguage(() => { if (localeChoice.value === LOCALE_SYSTEM) { resolved.value = systemLocale(); selectedLocale.value = resolved.value; changeLanguage() } })
</script>

<style>
.welcome-lang { position: relative; display: inline-flex; align-items: center; gap: 8px; height: 40px; padding: 0 16px; border-radius: 9999px; background: var(--md-sys-color-surface-container-highest); color: var(--md-sys-color-on-surface); cursor: pointer; }
.welcome-lang:hover { background: color-mix(in srgb, var(--md-sys-color-on-surface) 8%, var(--md-sys-color-surface-container-highest)); }
.welcome-lang:focus-within { outline: 2px solid var(--md-sys-color-primary); }
.welcome-lang select { appearance: none; border: 0; background: transparent; color: inherit; font: var(--m-label-large); cursor: pointer; outline: 0; padding-inline-end: 4px; }
.welcome-lang select option { color: initial; }
</style>
