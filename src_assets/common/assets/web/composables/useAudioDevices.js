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

  const refresh = async () => {
    if (platform !== 'windows' || pending) return
    pending = true
    try {
      const payload = await apiJson('/api/audio/devices')
      if (!active) return
      state.value = normalizeAudioDevices(payload)
      failed.value = payload?.success === false
      watchJob()
    } catch {
      if (active) failed.value = true
    } finally {
      pending = false
      if (active) loaded.value = true
    }
  }
  const onVisible = () => { if (!document.hidden) void refresh() }
  // while the host installs or enables VB-CABLE the page asks for its progress every second and a half
  let jobTimer
  const watchJob = () => {
    window.clearTimeout(jobTimer)
    if (active && state.value.vb.job.running) jobTimer = window.setTimeout(() => { void refresh() }, 1500)
  }

  onMounted(() => {
    active = true
    void refresh()
    window.addEventListener('focus', onVisible)
    document.addEventListener('visibilitychange', onVisible)
  })
  onBeforeUnmount(() => {
    active = false
    window.clearTimeout(jobTimer)
    window.removeEventListener('focus', onVisible)
    document.removeEventListener('visibilitychange', onVisible)
  })

  return {
    render: computed(() => state.value.render),
    capture: computed(() => state.value.capture),
    vb: computed(() => state.value.vb),
    loaded,
    failed,
    refresh,
  }
}
