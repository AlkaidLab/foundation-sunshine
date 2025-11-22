import { ref, computed, onUnmounted } from 'vue'

/**
 * 故障排除组合式函数
 */
export function useTroubleshooting() {
  const platform = ref('')
  const closeAppPressed = ref(false)
  const closeAppStatus = ref(null)
  const restartPressed = ref(false)
  const boomPressed = ref(false)
  const resetDisplayDevicePressed = ref(false)
  const resetDisplayDeviceStatus = ref(null)
  const logs = ref('Loading...')
  const logFilter = ref(null)
  const logInterval = ref(null)

  // 过滤后的日志
  const actualLogs = computed(() => {
    if (!logFilter.value) return logs.value
    const lines = logs.value.split('\n')
    return lines.filter(line => line.indexOf(logFilter.value) !== -1).join('\n')
  })

  // 刷新日志
  const refreshLogs = async () => {
    try {
      const response = await fetch('/api/logs')
      logs.value = await response.text()
    } catch (error) {
      console.error('Failed to refresh logs:', error)
      logs.value = 'Failed to load logs'
    }
  }

  // 关闭应用
  const closeApp = async () => {
    closeAppPressed.value = true
    try {
      const response = await fetch('/api/apps/close', { method: 'POST' })
      const data = await response.json()
      closeAppStatus.value = data.status.toString() === 'true'
      setTimeout(() => {
        closeAppStatus.value = null
      }, 5000)
    } catch (error) {
      console.error('Failed to close app:', error)
      closeAppStatus.value = false
    } finally {
      closeAppPressed.value = false
    }
  }

  // 重启 Sunshine
  const restart = async () => {
    restartPressed.value = true
    try {
      await fetch('/api/restart', { method: 'POST' })
      setTimeout(() => {
        restartPressed.value = false
      }, 5000)
    } catch (error) {
      console.error('Failed to restart:', error)
      restartPressed.value = false
    }
  }

  // 确认 Boom
  const confirmBoom = () => {
    const modal = document.getElementById('boomModal')
    if (modal) {
      modal.classList.add('show')
    }
  }

  // 关闭模态框
  const closeModal = () => {
    const modal = document.getElementById('boomModal')
    if (modal) {
      modal.classList.remove('show')
    }
  }

  // Boom Sunshine
  const boom = async () => {
    boomPressed.value = true
    closeModal()
    try {
      await fetch('/api/boom', { method: 'GET' })
    } catch (error) {
      console.error('Failed to boom:', error)
    }
  }

  // 重置显示器设备持久化
  const resetDisplayDevicePersistence = async () => {
    resetDisplayDevicePressed.value = true
    try {
      const response = await fetch('/api/reset-display-device-persistence', { method: 'POST' })
      const data = await response.json()
      resetDisplayDeviceStatus.value = data.status.toString() === 'true'
      setTimeout(() => {
        resetDisplayDeviceStatus.value = null
      }, 5000)
    } catch (error) {
      console.error('Failed to reset display device:', error)
      resetDisplayDeviceStatus.value = false
    } finally {
      resetDisplayDevicePressed.value = false
    }
  }

  // 复制日志
  const copyLogs = async () => {
    try {
      await navigator.clipboard.writeText(actualLogs.value)
    } catch (error) {
      console.error('Failed to copy logs:', error)
    }
  }

  // 复制配置
  const copyConfig = async (t) => {
    try {
      const response = await fetch('/api/config')
      const data = await response.json()
      const configString = JSON.stringify(data, null, 2)
      await navigator.clipboard.writeText(configString)
      alert(t('troubleshooting.copy_config_success'))
    } catch (error) {
      console.error('Failed to copy config:', error)
      alert(t('troubleshooting.copy_config_error'))
    }
  }

  // 重新打开设置向导
  const reopenSetupWizard = async (t) => {
    try {
      const response = await fetch('/api/config')
      const config = await response.json()
      config.setup_wizard_completed = false
      
      const saveResponse = await fetch('/api/config', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(config),
      })
      
      if (saveResponse.ok) {
        window.location.href = '/'
      } else {
        alert(t('troubleshooting.reopen_setup_wizard_error'))
      }
    } catch (error) {
      console.error('Failed to reopen setup wizard:', error)
      alert(t('troubleshooting.reopen_setup_wizard_error'))
    }
  }

  // 加载平台信息
  const loadPlatform = async () => {
    try {
      const response = await fetch('/api/config')
      const data = await response.json()
      platform.value = data.platform || ''
    } catch (error) {
      console.error('Failed to load platform:', error)
    }
  }

  // 启动日志自动刷新
  const startLogRefresh = () => {
    logInterval.value = setInterval(() => {
      refreshLogs()
    }, 5000)
  }

  // 停止日志自动刷新
  const stopLogRefresh = () => {
    if (logInterval.value) {
      clearInterval(logInterval.value)
      logInterval.value = null
    }
  }

  // 清理
  onUnmounted(() => {
    stopLogRefresh()
  })

  return {
    platform,
    closeAppPressed,
    closeAppStatus,
    restartPressed,
    boomPressed,
    resetDisplayDevicePressed,
    resetDisplayDeviceStatus,
    logs,
    logFilter,
    actualLogs,
    refreshLogs,
    closeApp,
    restart,
    confirmBoom,
    closeModal,
    boom,
    resetDisplayDevicePersistence,
    copyLogs,
    copyConfig,
    reopenSetupWizard,
    loadPlatform,
    startLogRefresh,
    stopLogRefresh,
  }
}

