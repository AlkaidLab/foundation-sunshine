import { computed, ref, watch } from 'vue'

// Changing the display itself: the resolution, the refresh rate and the scaling of the display that is streamed (or the one the person chose). The browser cannot do it; the
// control panel can, through two commands of its own (list_display_modes / set_display_mode) and the DPI path it already has (get_current_dpi / set_desktop_dpi). In the panel's
// frame the commands are `window.__TAURI__.core.invoke`; in a browser there are none and the card shows the values without the sliders.
const invoker = () => (typeof window !== 'undefined' ? window.__TAURI__?.core?.invoke : null)

/** `\\.\DISPLAY3` of a display the host lists ("DISPLAY NAME: …"), '' when the data names none. */
export const gdiNameOf = (data = '') => {
  const m = /(DISPLAY\d+)/.exec(String(data))
  return m ? `\\\\.\\${m[1]}` : ''
}

/** The scaling stops of the card: 100 % to 300 % in steps of 25, as the DPI tool offers. */
export const SCALES = Object.freeze([100, 125, 150, 175, 200, 225, 250, 275, 300])

/**
 * `target`: a getter for the GDI name of the display the card applies to ('' = the main display). Returns the model of the card, the supported stops, and `apply`.
 * `modes` is the list the host reports: [{ width, height, hz: [..] }]; the card's stops are derived from it.
 */
export function useDisplayLive(target) {
  const invoke = invoker()
  const live = !!invoke
  const modes = ref([])
  const model = ref({ resolution: '', refresh: 0, scale: 100 })
  const state = ref('')
  const error = ref('')
  const loading = ref(false)

  const resolutions = computed(() => [...new Set(modes.value.map((m) => `${m.width}x${m.height}`))].sort((a, b) => { const [aw, ah] = a.split('x').map(Number); const [bw, bh] = b.split('x').map(Number); return aw * ah - bw * bh }))
  const refreshes = computed(() => {
    const [w, h] = String(model.value.resolution).split('x').map(Number)
    const entry = modes.value.find((m) => m.width === w && m.height === h)
    return [...(entry?.hz || [])].sort((a, b) => a - b)
  })

  async function load() {
    if (!invoke) return
    loading.value = true
    try {
      const answer = await invoke('list_display_modes', { device: target() || null })
      modes.value = Array.isArray(answer?.modes) ? answer.modes : []
      const cur = answer?.current
      if (cur) model.value = { ...model.value, resolution: `${cur.width}x${cur.height}`, refresh: cur.hz }
    } catch (e) { modes.value = []; error.value = String(e) }
    try { const dpi = await invoke('get_current_dpi'); if (dpi) model.value = { ...model.value, scale: SCALES.includes(dpi) ? dpi : Math.min(300, Math.max(100, Math.round(dpi / 25) * 25)) } } catch { /* the scaling stays at 100 */ }
    loading.value = false
  }
  watch(target, load)

  // choosing a resolution: keep the refresh rate when that resolution has it, else the nearest one it has
  watch(() => model.value.resolution, () => {
    const list = refreshes.value
    const now = model.value.refresh
    if (list.length && !list.some((r) => Math.abs(r - now) < 0.5)) model.value = { ...model.value, refresh: list.reduce((best, r) => (Math.abs(r - now) < Math.abs(best - now) ? r : best), list[0]) }
  })

  async function apply({ kind }) {
    if (!invoke) return
    state.value = 'applying'
    error.value = ''
    try {
      if (kind === 'scale') await invoke('set_desktop_dpi', { dpi: model.value.scale })
      else {
        const [width, height] = String(model.value.resolution).split('x').map(Number)
        await invoke('set_display_mode', { device: target() || null, width, height, hz: Math.round(model.value.refresh) })
      }
      state.value = 'applied'
      setTimeout(() => { if (state.value === 'applied') state.value = '' }, 2500)
    } catch (e) { state.value = 'failed'; error.value = String(e) }
  }

  return { live, model, resolutions, refreshes, state, error, loading, load, apply }
}
