import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { copyFor } from '../utils/sparxie/copy.js'

/** The words of the Sparxie pages in the language of the Web UI (a ref: use it as `C.title` in a template, `C.value` in a script) */
export function useSparxieCopy() {
  const { locale } = useI18n()
  return computed(() => copyFor(locale.value))
}
