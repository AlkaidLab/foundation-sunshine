import { shallowReactive } from 'vue'
import { canonicalLocale, resolveTexts } from 'foundation-ui/locale/texts/resolve.js'

// English and Simplified Chinese are built into each component. Other locales
// add one shared dictionary, including Traditional Chinese over the Chinese base.
export function createLocalePacks(loaders) {
  const PARTIALS = shallowReactive({})
  const pending = new Map()
  const attempted = new Set()

  function loadLocale(locale) {
    const code = canonicalLocale(locale)
    if (!loaders[code] || PARTIALS[code]) return
    if (!pending.has(code)) {
      attempted.add(code)
      pending.set(code, loaders[code]()
        .then((pack) => { PARTIALS[code] = pack })
        .catch((error) => { console.warn('Could not load shared UI texts for ' + code, error) })
        .finally(() => pending.delete(code)))
    }
    return pending.get(code)
  }

  function partialFor(locale, name) {
    const code = canonicalLocale(locale)
    // A component may independently choose a language (e.g. the welcome
    // wizard). Its computed text updates once the overlay is available.
    if (loaders[code] && !attempted.has(code)) void loadLocale(code)
    return PARTIALS[code]?.[name] || null
  }

  const textTable = (name, locale, en, zh) => resolveTexts(en, zh, locale, partialFor(locale, name))
  return { PARTIALS, loadLocale, partialFor, textTable }
}
