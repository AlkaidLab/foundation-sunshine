import { watchEffect } from 'vue'

// The words of the options of a setting, as the page shows them (the locale keys differ per platform and per option, so the unsaved list can not rebuild them).
// A page that renders a choice registers its options; the copy in sessionStorage keeps them for entries that outlive the page or a reload.
const KEY = 'foundation.optionLabels'
let known = {}
try { known = JSON.parse(globalThis.sessionStorage?.getItem(KEY) || '{}') || {} } catch { known = {} }

export function registerOptionLabels(key, options) {
  const map = {}
  for (const o of options || []) if (o && o.label != null && o.label !== '') map[String(o.value)] = String(o.label)
  if (JSON.stringify(known[key]) === JSON.stringify(map)) return
  known[key] = map
  try { globalThis.sessionStorage?.setItem(KEY, JSON.stringify(known)) } catch { /* private mode */ }
}
export const optionLabelOf = (key, value) => known[key]?.[String(value)]

/** for the setup of a component that gets `k` and `options` as props */
export function useOptionLabels(props) {
  watchEffect(() => registerOptionLabels(props.k, props.options))
}
