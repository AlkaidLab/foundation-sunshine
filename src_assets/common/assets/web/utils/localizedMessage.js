import en from '../public/assets/locale/en.json' with { type: 'json' }

// Keep the English fallback for utilities outside Vue in the UI catalog.
export function localizedMessage(translate, key, params = {}) {
  if (translate) return translate(key, params)
  const message = key.split('.').reduce((value, part) => value?.[part], en)
  return (message ?? key).replace(/\{(\w+)\}/g, (match, name) => params[name] ?? match)
}
