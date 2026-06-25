const LANGUAGE_NAMES = {
  bg: 'Bulgarian',
  cs: 'Czech',
  de: 'German',
  en: 'English',
  en_GB: 'British English',
  en_US: 'American English',
  es: 'Spanish',
  fr: 'French',
  it: 'Italian',
  ja: 'Japanese',
  ko: 'Korean',
  pl: 'Polish',
  pt: 'Portuguese',
  pt_BR: 'Brazilian Portuguese',
  ru: 'Russian',
  sv: 'Swedish',
  tr: 'Turkish',
  uk: 'Ukrainian',
  zh: 'Simplified Chinese',
  zh_TW: 'Traditional Chinese',
}

function normalizeLocale(raw) {
  if (!raw || typeof raw !== 'string') return 'en'

  const value = raw.replace('-', '_')
  const lower = value.toLowerCase()

  if (lower.startsWith('zh') && /(_tw|_hk|_mo|hant)/.test(lower)) {
    return 'zh_TW'
  }
  if (lower.startsWith('zh')) {
    return 'zh'
  }
  if (lower === 'pt_br') {
    return 'pt_BR'
  }
  if (lower === 'en_gb') {
    return 'en_GB'
  }
  if (lower === 'en_us') {
    return 'en_US'
  }

  const primary = value.split('_')[0]
  return LANGUAGE_NAMES[primary] ? primary : 'en'
}

export function getCurrentLocale() {
  if (typeof document !== 'undefined') {
    const htmlLang = document.documentElement?.getAttribute('lang')
    if (htmlLang) return normalizeLocale(htmlLang)
  }

  if (typeof navigator !== 'undefined') {
    if (Array.isArray(navigator.languages) && navigator.languages.length > 0) {
      return normalizeLocale(navigator.languages[0])
    }
    if (navigator.language) {
      return normalizeLocale(navigator.language)
    }
  }

  return 'en'
}

export function getPromptLanguageName(locale = getCurrentLocale()) {
  const normalized = normalizeLocale(locale)
  return LANGUAGE_NAMES[normalized] || LANGUAGE_NAMES.en
}

export function buildLocalizedInstruction(locale = getCurrentLocale()) {
  const languageName = getPromptLanguageName(locale)
  return `Use ${languageName} for user-facing explanations and short reasons.`
}
