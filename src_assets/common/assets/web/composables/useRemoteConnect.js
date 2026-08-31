import { ref } from 'vue'
import { apiJson, apiPostJson } from '../utils/apiFetch.js'

export function useRemoteConnect() {
  const enabled = ref(false)
  const running = ref(false)
  const available = ref(true)
  const busy = ref(false)
  const error = ref('')

  const applyStatus = (data) => {
    enabled.value = Boolean(data.enabled)
    running.value = Boolean(data.running)
    available.value = data.available !== false
    error.value = data.error || ''
    return data.status !== false
  }

  const loadStatus = async () => {
    busy.value = true
    try {
      applyStatus(await apiJson('/api/remote-connect'))
    } catch (requestError) {
      error.value = requestError?.message || 'Unable to read remote connection status'
    } finally {
      busy.value = false
    }
  }

  const setEnabled = async (nextEnabled) => {
    busy.value = true
    error.value = ''
    try {
      const data = await apiPostJson('/api/remote-connect', { enabled: nextEnabled })
      return applyStatus(data)
    } catch (requestError) {
      error.value = requestError?.message || 'Unable to update remote connection'
      return false
    } finally {
      busy.value = false
    }
  }

  const reset = async () => {
    busy.value = true
    error.value = ''
    try {
      const data = await apiPostJson('/api/remote-connect/reset')
      return applyStatus(data)
    } catch (requestError) {
      error.value = requestError?.message || 'Unable to reset remote access'
      return false
    } finally {
      busy.value = false
    }
  }

  return {
    enabled,
    running,
    available,
    busy,
    error,
    loadStatus,
    setEnabled,
    reset,
  }
}
