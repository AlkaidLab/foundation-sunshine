import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { apiJson } from '../utils/apiFetch.js'
import { normalizeAudioDevices } from '../utils/audioDevices.js'

/**
 * The audio endpoints of the host (GET /api/audio/devices), read when the page opens and again when the window gets the
 * focus back: a person installs VB-CABLE or plugs in a device elsewhere and returns here. Only Windows lists devices.
 */
export function useAudioDevices(platform) {
  const state = ref(normalizeAudioDevices(null))
  const loaded = ref(false)
  const failed = ref(false)
  let active = false
  let pending = false
  let waitTimer
  // the last good answer said there is no default playback device: the page keeps looking, also after a read that failed
  let lastMissing = false

  const refresh = async () => {
    if (platform !== 'windows' || pending) return
    pending = true
    try {
      const payload = await apiJson('/api/audio/devices')
      if (!active) return
      state.value = normalizeAudioDevices(payload)
      failed.value = payload?.success === false
      if (!failed.value) lastMissing = state.value.supported && state.value.defaultOutput.present === false
    } catch {
      if (active) failed.value = true
    } finally {
      pending = false
      if (active) loaded.value = true
      watchDefault()
    }
  }
  const onVisible = () => { if (!document.hidden) void refresh() }
  // with no default playback device the page looks again every few seconds: a device plugged in or enabled elsewhere shows up here and the notice goes away
  const watchDefault = () => {
    window.clearTimeout(waitTimer)
    if (active && lastMissing) {
      waitTimer = window.setTimeout(() => { if (!document.hidden) void refresh(); else watchDefault() }, 5000)
    }
  }

  onMounted(() => {
    active = true
    void refresh()
    window.addEventListener('focus', onVisible)
    document.addEventListener('visibilitychange', onVisible)
  })
  onBeforeUnmount(() => {
    active = false
    window.clearTimeout(waitTimer)
    window.removeEventListener('focus', onVisible)
    document.removeEventListener('visibilitychange', onVisible)
  })

  return {
    render: computed(() => state.value.render),
    capture: computed(() => state.value.capture),
    vb: computed(() => state.value.vb),
    defaultOutput: computed(() => state.value.defaultOutput),
    supported: computed(() => state.value.supported),
    loaded,
    failed,
    refresh,
  }
}
