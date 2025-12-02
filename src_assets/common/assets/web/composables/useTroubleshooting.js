import { ref, computed, onUnmounted } from 'vue'

const LOG_REFRESH_INTERVAL = 5000
const STATUS_RESET_DELAY = 5000

/**
 * Creates a delayed status reset helper
 */
const createStatusResetter = (statusRef) => {
  return setTimeout(() => {
    statusRef.value = null
  }, STATUS_RESET_DELAY)
}

/**
 * Wraps an async action with pressed state management
 */
const withPressedState = async (pressedRef, action, autoReset = false) => {
  pressedRef.value = true
  try {
    return await action()
  } finally {
    if (autoReset) {
      setTimeout(() => {
        pressedRef.value = false
      }, STATUS_RESET_DELAY)
    } else {
      pressedRef.value = false
    }
  }
}

/**
 * Troubleshooting composable
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
  const matchMode = ref('contains')
  const ignoreCase = ref(true)
  const logInterval = ref(null)

  const actualLogs = computed(() => {
    if (!logFilter.value) return logs.value

    const filter = ignoreCase.value ? logFilter.value.toLowerCase() : logFilter.value
    const lines = logs.value.split('\n')

    const filterFn = (() => {
      switch (matchMode.value) {
        case 'exact':
          return (line) => {
            const searchLine = ignoreCase.value ? line.toLowerCase() : line
            return searchLine === filter
          }
        case 'regex':
          try {
            const regex = new RegExp(logFilter.value, ignoreCase.value ? 'i' : '')
            return (line) => regex.test(line)
          } catch {
            return () => false
          }
        default:
          return (line) => {
            const searchLine = ignoreCase.value ? line.toLowerCase() : line
            return searchLine.includes(filter)
          }
      }
    })()

    return lines.filter(filterFn).join('\n')
  })

  const fetchJson = async (url, options = {}) => {
    const response = await fetch(url, options)
    return response.json()
  }

  const refreshLogs = async () => {
    try {
      const response = await fetch('/api/logs')
      logs.value = await response.text()
    } catch {
      logs.value = 'Failed to load logs'
    }
  }

  const closeApp = () =>
    withPressedState(closeAppPressed, async () => {
      try {
        const data = await fetchJson('/api/apps/close', { method: 'POST' })
        closeAppStatus.value = data.status.toString() === 'true'
        createStatusResetter(closeAppStatus)
      } catch {
        closeAppStatus.value = false
      }
    })

  const restart = () =>
    withPressedState(
      restartPressed,
      async () => {
        try {
          await fetch('/api/restart', { method: 'POST' })
        } catch {}
      },
      true
    )

  const boom = async () => {
    boomPressed.value = true
    try {
      await fetch('/api/boom')
    } catch {}
  }

  const resetDisplayDevicePersistence = () =>
    withPressedState(resetDisplayDevicePressed, async () => {
      try {
        const data = await fetchJson('/api/reset-display-device-persistence', { method: 'POST' })
        resetDisplayDeviceStatus.value = data.status.toString() === 'true'
        createStatusResetter(resetDisplayDeviceStatus)
      } catch {
        resetDisplayDeviceStatus.value = false
      }
    })

  const copyLogs = async () => {
    try {
      await navigator.clipboard.writeText(actualLogs.value)
    } catch {}
  }

  const copyConfig = async (t) => {
    try {
      const data = await fetchJson('/api/config')
      await navigator.clipboard.writeText(JSON.stringify(data, null, 2))
      alert(t('troubleshooting.copy_config_success'))
    } catch {
      alert(t('troubleshooting.copy_config_error'))
    }
  }

  const reopenSetupWizard = async (t) => {
    try {
      const config = await fetchJson('/api/config')
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
      const data = await fetchJson('/api/config')
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
    matchMode,
    ignoreCase,
    actualLogs,
    refreshLogs,
    closeApp,
    restart,
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
