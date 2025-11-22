import { ref, computed } from 'vue'

/**
 * 日志管理组合式函数
 */
export function useLogs() {
  const logs = ref(null)

  // 解析日志文本，提取时间戳、级别和内容
  const fancyLogs = computed(() => {
    if (!logs.value) return []
    const regex = /(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}]):\s/g
    const rawLogLines = logs.value.split(regex).splice(1)
    const logLines = []
    
    for (let i = 0; i < rawLogLines.length; i += 2) {
      logLines.push({
        timestamp: rawLogLines[i],
        level: rawLogLines[i + 1]?.split(':')[0] || 'Unknown',
        value: rawLogLines[i + 1] || '',
      })
    }
    return logLines
  })

  // 获取致命错误日志
  const fatalLogs = computed(() => {
    return fancyLogs.value.filter(log => log.level === 'Fatal')
  })

  // 获取日志
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

