import { onBeforeUnmount, ref } from 'vue'
import { apiFetch } from '../utils/apiFetch.js'

// The live sessions of the host, for pages that show one line about them. /api/runtime/sessions answers only on the
// host itself: a 403 stops the polling (nothing is faked), the page keeps whatever it last saw. One request every
// five seconds while the page is open; nothing is asked while the tab is hidden.
export function useRuntimeSessions(intervalMs = 5000) {
  const sessions = ref([])
  let timer = 0
  let closed = false

  async function tick() {
    if (!document.hidden) {
      try {
        const response = await apiFetch('/api/runtime/sessions')
        if (response.status === 403 || response.status === 401) {
          closed = true
          sessions.value = []
          return
        }
        if (response.ok) {
          const data = await response.json().catch(() => null)
          sessions.value = Array.isArray(data?.sessions) ? data.sessions : []
        }
      } catch { /* the host is busy: the next round retries */ }
    }
    if (!closed) timer = setTimeout(tick, intervalMs)
  }

  timer = setTimeout(tick, 0)
  onBeforeUnmount(() => {
    closed = true
    clearTimeout(timer)
  })
  return { sessions }
}
