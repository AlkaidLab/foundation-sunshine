import { ref } from 'vue'
import { apiJson } from '../utils/apiFetch.js'

export function useMatLiveStatus() {
  const status = ref(null)
  const loading = ref(false)
  let active = true
  const refresh = async () => {
    if (!active || loading.value) return
    loading.value = true
    try {
      const next = await apiJson('/api/mat/status', { cache: 'no-store' })
      if (active) status.value = next
    } catch {
      if (active) status.value = { state: 'unavailable', last_error: '' }
    } finally {
      loading.value = false
    }
  }
  return { status, loading, refresh, dispose: () => { active = false } }
}
