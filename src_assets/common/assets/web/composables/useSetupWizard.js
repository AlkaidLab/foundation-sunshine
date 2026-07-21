import { ref } from 'vue'

export const SETUP_WIZARD_LANGUAGE_SAVED_KEY = 'sunshine_setup_wizard_language_saved'

function isTrue(value) {
  return value === true || value === 'true'
}

function getSessionStorage() {
  try {
    return typeof window !== 'undefined' ? window.sessionStorage : globalThis.sessionStorage
  } catch (e) {
    return null
  }
}

function hasSavedLanguageThisSession() {
  return getSessionStorage()?.getItem(SETUP_WIZARD_LANGUAGE_SAVED_KEY) === 'true'
}

function clearSavedLanguageMarker() {
  getSessionStorage()?.removeItem(SETUP_WIZARD_LANGUAGE_SAVED_KEY)
}

function isChineseBrowserLocale() {
  try {
    const nav = typeof navigator !== 'undefined' ? navigator : null
    const locales = [
      ...(Array.isArray(nav?.languages) ? nav.languages : []),
      nav?.language,
    ].filter(Boolean)

    return locales.some((locale) => String(locale).toLowerCase().replace('-', '_').startsWith('zh'))
  } catch (e) {
    return false
  }
}

function shouldSkipLanguageStep(config) {
  if (hasSavedLanguageThisSession()) return true
  if (!config?.locale) return false
  return !(config.locale === 'en' && isChineseBrowserLocale())
}

/**
 * 设置向导组合式函数
 */
export function useSetupWizard() {
  const showSetupWizard = ref(false)
  const adapters = ref([])
  const displayDevices = ref([])
  const hasLocale = ref(false)

  // 检查是否需要显示设置向导
  const checkSetupWizard = (config) => {
    const setupCompleted = isTrue(config.setup_wizard_completed)
    
    if (setupCompleted) {
      clearSavedLanguageMarker()
      return false
    }

    showSetupWizard.value = true
    adapters.value = config.adapters || []
    displayDevices.value = config.display_devices || []
    hasLocale.value = shouldSkipLanguageStep(config)
    return true
  }

  // 设置完成回调
  const onSetupComplete = (config) => {
    console.log('设置完成:', config)
    // 用户点击"配置应用程序"按钮后会自动跳转到 /apps
  }

  return {
    showSetupWizard,
    adapters,
    displayDevices,
    hasLocale,
    checkSetupWizard,
    onSetupComplete,
  }
}

