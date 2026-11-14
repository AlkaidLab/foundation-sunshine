import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { pinSpecForSetting } from 'foundation-ui/vue'

/** What a configuration row can be pinned to the overview as (its key, title and icon): null for a key the pins do not know. */
export function useCfgPin(props, title) {
  const { locale } = useI18n()
  return computed(() => pinSpecForSetting(props.k, { title: title.value, icon: props.icon, locale: String(locale.value) }))
}
