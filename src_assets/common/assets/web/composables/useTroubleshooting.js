import { ref, computed, onUnmounted } from 'vue'

const LOG_REFRESH_INTERVAL = 5000
const STATUS_RESET_DELAY = 5000

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

  const actualLogs = computed(() => {
    if (!logFilter.value) return logs.value
    return logs.value
      .split('\n')
      .filter((line) => line.includes(logFilter.value))
      .join('\n')
  })

  const refreshLogs = async () => {
    try {
      const response = await fetch('/api/logs')
      logs.value = await response.text()
    } catch {
      logs.value = 'Failed to load logs'
    }
  }

  const closeApp = async () => {
    closeAppPressed.value = true
    try {
      const response = await fetch('/api/apps/close', { method: 'POST' })
      const data = await response.json()
      closeAppStatus.value = data.status.toString() === 'true'
      setTimeout(() => {
        closeAppStatus.value = null
      }, STATUS_RESET_DELAY)
    } catch {
      closeAppStatus.value = false
    } finally {
      closeAppPressed.value = false
    }
  }

  const restart = async () => {
    restartPressed.value = true
    try {
      await fetch('/api/restart', { method: 'POST' })
    } catch {}
    setTimeout(() => {
      restartPressed.value = false
    }, STATUS_RESET_DELAY)
  }

  const confirmBoom = () => document.getElementById('boomModal')?.classList.add('show')
  const closeModal = () => document.getElementById('boomModal')?.classList.remove('show')

  const boom = async () => {
    boomPressed.value = true
    closeModal()
    try {
      await fetch('/api/boom')
    } catch {}
  }

  const resetDisplayDevicePersistence = async () => {
    resetDisplayDevicePressed.value = true
    try {
      const response = await fetch('/api/reset-display-device-persistence', { method: 'POST' })
      const data = await response.json()
      resetDisplayDeviceStatus.value = data.status.toString() === 'true'
      setTimeout(() => {
        resetDisplayDeviceStatus.value = null
      }, STATUS_RESET_DELAY)
    } catch {
      resetDisplayDeviceStatus.value = false
    } finally {
      resetDisplayDevicePressed.value = false
    }
  }

  const copyLogs = async () => {
    try {
      await navigator.clipboard.writeText(actualLogs.value)
    } catch {}
  }

  const copyConfig = async (t) => {
    try {
      const response = await fetch('/api/config')
      const data = await response.json()
      await navigator.clipboard.writeText(JSON.stringify(data, null, 2))
      alert(t('troubleshooting.copy_config_success'))
    } catch {
      alert(t('troubleshooting.copy_config_error'))
    }
  }

  const reopenSetupWizard = async (t) => {
    try {
      const response = await fetch('/api/config')
      const config = await response.json()
      config.setup_wizard_completed = false

      const saveResponse = await fetch('/api/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(config),
      })

      if (saveResponse.ok) {
        window.location.href = '/'
      } else {
        alert(t('troubleshooting.reopen_setup_wizard_error'))
      }
    } catch {
      alert(t('troubleshooting.reopen_setup_wizard_error'))
    }
  }

  const loadPlatform = async () => {
    try {
      const response = await fetch('/api/config')
      const data = await response.json()
      platform.value = data.platform || ''
    } catch {}
  }

  const startLogRefresh = () => {
    logInterval.value = setInterval(refreshLogs, LOG_REFRESH_INTERVAL)
  }

  const stopLogRefresh = () => {
    if (logInterval.value) {
      clearInterval(logInterval.value)
      logInterval.value = null
    }
  }

  onUnmounted(stopLogRefresh)

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
