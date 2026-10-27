import { ref } from 'vue'
import { apiJson } from '../utils/apiFetch.js'

export function useMatLiveStatus() {
  const status = ref(null)
  const loading = ref(false)
  let active = true
  let timer
  const refresh = async () => {
    if (!active || loading.value) return
    loading.value = true
    try {
      const next = await apiJson('/api/mat/status', { cache: 'no-store', signal: AbortSignal.timeout(2500) })
      if (active) status.value = next
    } catch {
      if (active) status.value = { state: 'unavailable', last_error: '' }
    } finally {
      loading.value = false
    }
  }
  const start = () => {
    if (!active || timer !== undefined) return
    const tick = () => { if (!document.hidden) void refresh() }
    tick()
    timer = setInterval(tick, 3000)
  }
  const dispose = () => {
    active = false
    clearInterval(timer)
  }
  return { status, loading, refresh, start, dispose }
}
