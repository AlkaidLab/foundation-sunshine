import { computed } from 'vue'

import { FORMATS, formatOf, hdrOf, modesFor } from 'foundation-ui/settings/index.js'

// The video format as one choice and "allow HDR" as one flag, over the two codec modes of the host (the pure part is foundation-ui settings/codecFormat.js).
export { FORMATS, formatOf, hdrOf, modesFor }

export function useCodecFormat(config) {
  const format = computed({
    get: () => formatOf(config),
    set: (value) => { Object.assign(config, modesFor(value, hdrOf(config))) },
  })
  const hdr = computed({
    get: () => hdrOf(config),
    set: (value) => { if (formatOf(config) !== 'h264') Object.assign(config, modesFor(formatOf(config), value)) },
  })
  return { format, hdr, hdrAvailable: computed(() => formatOf(config) !== 'h264') }
}
