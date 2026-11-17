import { computed } from 'vue'

// Shared reading and writing of one configuration key by the Cfg* rows: the value as the control sees it and as the
// configuration keeps it. The configuration keeps what the host file keeps (strings 'enabled'/'disabled', numbers as
// text or numbers), so every comparison is as text.
export const asText = (value) => (value === undefined || value === null ? '' : String(value))

/** switch value: true when the stored value equals `on`; an unset key is `def` (then `off`) */
export function useSwitchModel(props) {
  return computed({
    get: () => String(props.config[props.k] ?? props.def ?? props.off) === String(props.on),
    set: (value) => { props.config[props.k] = value ? props.on : props.off },
  })
}

/** index of a named step: a slider over `steps` has the position as its value */
export const stepIndex = (steps, stored, def) => {
  const at = steps.findIndex((s) => String(s) === String(stored ?? def))
  return at < 0 ? Math.max(0, steps.findIndex((s) => String(s) === String(def))) : at
}
