import { ref, computed } from 'vue'

const LOG_REGEX = /(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}]):\s/g

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
      const response = await fetch('/api/logs')
      logs.value = await response.text()
    } catch (e) {
      console.error('Failed to fetch logs:', e)
    }
  }

  return {
    logs,
    fancyLogs,
    fatalLogs,
    fetchLogs,
  }
}
