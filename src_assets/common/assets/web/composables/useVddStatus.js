import { computed, ref } from 'vue'
import { apiJson } from '../utils/apiFetch.js'

const EMPTY_STATUS = Object.freeze({
  state: 'unknown',
  installed: false,
  running: false,
  control_available: false,
  installed_version: '',
  bundled_version: '',
  version_match: false,
  monitor_active: false,
  problem_code: 0,
  status_text: '',
})

function normalizeStatus(value) {
  const raw = value?.success === true && value?.data ? value.data : value
  return raw && typeof raw === 'object'
    ? { ...EMPTY_STATUS, ...raw }
    : { ...EMPTY_STATUS }
}

export function useVddStatus() {
  const vddStatus = ref({ ...EMPTY_STATUS })
  const vddStatusLoading = ref(false)
  const vddInstalling = ref(false)
  const vddStatusError = ref('')

  const canManageVdd = computed(() =>
    typeof window !== 'undefined' && typeof window.vddDriver?.install === 'function'
  )
  const vddReady = computed(() =>
    vddStatus.value.installed &&
    vddStatus.value.running &&
    ['ready', 'degraded'].includes(vddStatus.value.state)
  )

  async function refreshVddStatus() {
    vddStatusLoading.value = true
    vddStatusError.value = ''
    try {
      const result = typeof window !== 'undefined' && typeof window.vddDriver?.getStatus === 'function'
        ? await window.vddDriver.getStatus()
        : await apiJson('/api/vdd/status')
      vddStatus.value = normalizeStatus(result)
      return vddStatus.value
    } catch (error) {
      vddStatus.value = { ...EMPTY_STATUS }
      vddStatusError.value = error instanceof Error ? error.message : String(error)
      return vddStatus.value
    } finally {
      vddStatusLoading.value = false
    }
  }

  async function installVdd() {
    if (!canManageVdd.value) {
      throw new Error('VDD installation is available in the Sunshine desktop app on the host.')
    }

    vddInstalling.value = true
    vddStatusError.value = ''
    try {
      const result = await window.vddDriver.install()
      vddStatus.value = normalizeStatus(result)
      if (!vddReady.value) {
        await refreshVddStatus()
      }
      return vddStatus.value
    } catch (error) {
      vddStatusError.value = error instanceof Error ? error.message : String(error)
      throw error
    } finally {
      vddInstalling.value = false
    }
  }

  return {
    vddStatus,
    vddStatusLoading,
    vddInstalling,
    vddStatusError,
    canManageVdd,
    vddReady,
    refreshVddStatus,
    installVdd,
  }
}
