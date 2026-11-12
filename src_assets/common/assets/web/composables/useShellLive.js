import { computed, ref } from 'vue'
import { apiJson } from '../utils/apiFetch.js'
import { encoderLabel, countApps, summariseClients } from '../utils/shellLive.js'

// Shared counters for the list pane and the home tiles. One poll for the whole page (10 s while the tab is visible);
// every consumer calls start() and stop(), the last stop ends the timer.
const clients = ref(null)
const apps = ref(null)
const encoder = ref('')
// the host's whole /api/config answer (the settings list shows live values of several categories) and the DualSense tuning keys (search)
const config = ref(null)
const dualsenseKeys = ref([])
// the DualSense channel and the microphone, for the second lines of their rows
const ds = ref(null)
const mic = ref(null)
let users = 0
let timer = 0

async function refresh() {
  if (typeof document !== 'undefined' && document.hidden) return
  const [c, a, cfg] = await Promise.allSettled([apiJson('/api/clients/list'), apiJson('/api/apps'), apiJson('/api/config')])
  if (c.status === 'fulfilled') clients.value = summariseClients(c.value)
  if (a.status === 'fulfilled') apps.value = countApps(a.value)
  if (cfg.status === 'fulfilled') { encoder.value = encoderLabel(cfg.value?.active_encoder); config.value = cfg.value }
  const [tuning, microphone] = await Promise.allSettled([apiJson('/api/dualsense/tuning'), apiJson('/api/microphone/status')])
  if (tuning.status === 'fulfilled' && tuning.value) {
    dualsenseKeys.value = tuning.value.keys || []
    ds.value = tuning.value.status ? { state: Number(tuning.value.active_count) > 0 ? 'active' : 'ready' } : null
  }
  if (microphone.status === 'fulfilled') mic.value = microphone.value
}

export function useShellLive() {
  return {
    clients,
    apps,
    encoder,
    config,
    dualsenseKeys,
    ds,
    mic,
    state: computed(() => ({ clients: clients.value && { total: clients.value.total, streaming: clients.value.streaming }, apps: apps.value, encoder: encoder.value })),
    refresh,
    start() {
      if (users++ === 0) { void refresh(); timer = setInterval(refresh, 10000) }
    },
    stop() {
      if (--users === 0) clearInterval(timer)
    },
  }
}
