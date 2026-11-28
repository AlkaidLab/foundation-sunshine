import { watch } from 'vue'
import { useSparxie } from './useSparxie.js'
import { useSparxieCopy } from './useSparxieCopy.js'
import { warnOutdated } from '../utils/sparxie/outdated.js'

/** For a page that shows Sparxie: warns when the installed release is outdated, as soon as the host's status says so */
export function useOutdatedWarning() {
  const C = useSparxieCopy()
  const { state } = useSparxie()
  watch(() => state.outdatedVersion, () => warnOutdated(state, C.value), { immediate: true })
}
