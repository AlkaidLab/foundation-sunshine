import { computed, inject } from 'vue'
import { useI18n } from 'vue-i18n'
import { $tp } from '../platform-i18n.js'

/**
 * Title and help text of a configuration row. The title key is `label` or `config.<k>`, the help key is `help` or
 * `config.<k>_desc` (shown when it exists; `help: 'none'` hides it). With `tp` the platform variant of a key
 * (`..._windows`, `..._unix`) is used when the locale has one.
 */
export function useCfgText(props) {
  const { t, te } = useI18n()
  const platform = inject('platform', null)
  const tr = (key) => (props.tp ? $tp(key, t(key)) : t(key))
  const has = (key) => te(key) || (props.tp && !!platform?.value && (te(`${key}_${platform.value}`) || te(`${key}_unix`)))
  const title = computed(() => tr(props.label || `config.${props.k}`))
  const info = computed(() => {
    const key = props.help === 'none' ? '' : props.help || `config.${props.k}_desc`
    return key && has(key) ? tr(key) : ''
  })
  return { t, tr, title, info }
}
