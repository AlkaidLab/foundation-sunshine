import { ref, computed } from 'vue'
import { readLogResponse } from 'foundation-ui/diagnostics'
import { apiFetch } from '../utils/apiFetch.js'

const LOG_REGEX = /(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}]):\s/g
let pendingTail = null

// The shell and overview can ask for the same snapshot while the page opens.
export function fetchLogTail() {
  if (!pendingTail) {
    pendingTail = apiFetch('/api/logs', { headers: { 'X-Log-Offset': '0' } })
      .then((response) => {
        if (!response.ok) throw new Error(`HTTP ${response.status}`)
        return readLogResponse(response)
      })
      .finally(() => { pendingTail = null })
  }
  return pendingTail
}

/**
 * 日志管理组合式函数
 */
export function useLogs() {
  const logs = ref(null)

  const fancyLogs = computed(() => {
    if (!logs.value) return []
    const parts = logs.value.split(LOG_REGEX).slice(1)
    const result = []
    for (let i = 0; i < parts.length; i += 2) {
      const content = parts[i + 1] || ''
      result.push({
        timestamp: parts[i],
        level: content.split(':')[0] || 'Unknown',
        value: content,
      })
    }
    return result
  })

  const fatalLogs = computed(() => fancyLogs.value.filter((log) => log.level === 'Fatal'))

  const fetchLogs = async () => {
    try {
      logs.value = await fetchLogTail()
      return true
    } catch (e) {
      console.error('Failed to fetch logs:', e)
      return false
    }
  }

  return {
    logs,
    fancyLogs,
    fatalLogs,
    fetchLogs,
  }
}
