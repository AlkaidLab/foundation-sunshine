import { ref } from 'vue'
import { getBootstrapConfig } from '../config/bootstrapData.js'
import { apiJson, apiPostJson } from '../utils/apiFetch.js'

const EDITABLE_CLIENT_FIELDS = ['name', 'hdrProfile', 'deviceSize', 'hdrBrightnessMode', 'hdrBrightnessMaxNits', 'hdrBrightnessMinNits', 'hdrBrightnessMaxFullFrameNits']

export const isColorProfileFile = (fileName) =>
  typeof fileName === 'string' && /\.(?:icc|icm)$/i.test(fileName)

export const formatHdrNits = (value, fractionDigits = 1, trimTrailingZeros = false) => {
  const number = Number(value)
  if (!Number.isFinite(number)) return '—'

  const formatted = number.toFixed(fractionDigits)
  return trimTrailingZeros
    ? formatted.replace(/(?:\.0+|(\.\d*?[1-9])0+)$/, '$1')
    : formatted
}

const isSuccessfulStatus = (status) => {
  const normalized = status?.toString().toLowerCase()
  return normalized === '1' || normalized === 'true'
}

/**
 * 客户端配对、权限与显示设置。
 */
export function usePin(translate = (_key, { error }) => `Client name saved, but display settings were not saved: ${error}`) {
  const pairingDeviceName = ref('')
  const config = ref(null)
  const clients = ref([])
  const hdrProfileList = ref([])
  const hasIccFileList = ref(false)
  const loading = ref(false)
  const saving = ref(false)
  const updating = ref(new Set())
  const clientError = ref('')
  const listError = ref('')
  const editingStates = {}
  const originalValues = {}

  const clearEditingState = (uuid) => {
    delete editingStates[uuid]
    delete originalValues[uuid]
  }

  const clearAllEditingStates = () => {
    Object.keys(editingStates).forEach(clearEditingState)
  }

  const parseClients = () => {
    try {
      return JSON.parse(config.value?.clients || '[]')
    } catch {
      return []
    }
  }

  const persistedClient = (client) => {
    const { hdrBrightnessRuntime: _runtime, enabled: _enabled, streaming: _streaming, ...settings } = client
    return settings
  }

  const refreshClients = async () => {
    loading.value = true
    try {
      const data = await apiJson('/api/clients/list')

      if (!isSuccessfulStatus(data.status) || !Array.isArray(data.named_certs)) throw new Error('Invalid client list response')

      const tmpClients = parseClients()
      clients.value = data.named_certs.map((client) => {
        const stored = tmpClients.find(({ uuid }) => uuid === client.uuid)
        const draft = editingStates[client.uuid] ? clients.value.find((item) => item.uuid === client.uuid) : null
        const merged = { ...(stored ? persistedClient(stored) : {}), ...(draft ? persistedClient(draft) : {}), ...client }
        merged.enabled = client.enabled !== false
        merged.hdrProfile ??= ''
        // 如果客户端没有deviceSize，设置默认值为medium
        if (!merged.deviceSize) {
          merged.deviceSize = 'medium'
        }
        merged.hdrBrightnessMode ||= 'auto'
        merged.hdrBrightnessMaxNits ??= 1000
        merged.hdrBrightnessMinNits ??= 0.001
        merged.hdrBrightnessMaxFullFrameNits ??= 1000
        return merged
      })
      listError.value = ''
      return true
    } catch (error) {
      console.error('Failed to refresh clients:', error)
      listError.value = error.message
      return false
    } finally {
      loading.value = false
    }
  }

  const setClientEnabled = async (uuid, enabled) => {
    if (updating.value.has(uuid)) return false
    updating.value.add(uuid)
    clientError.value = ''
    try {
      const data = await apiPostJson('/api/clients/update', { uuid, enabled })
      if (!isSuccessfulStatus(data.status)) throw new Error(data.error || 'Client permission update failed')
      const client = clients.value.find((item) => item.uuid === uuid)
      if (client) {
        client.enabled = enabled
        if (!enabled) client.streaming = false
      }
      return true
    } catch (error) {
      clientError.value = error.message
      return false
    } finally {
      updating.value.delete(uuid)
    }
  }

  const unpairAll = async () => {
    clientError.value = ''
    try {
      const data = await apiJson('/api/clients/unpair-all', { method: 'POST' })
      if (!isSuccessfulStatus(data.status)) throw new Error(data.error || 'Failed to unpair clients')
      clearAllEditingStates()
      await refreshClients()
      return true
    } catch (error) {
      console.error('Failed to unpair all:', error)
      clientError.value = error.message
      return false
    }
  }

  const unpairSingle = async (uuid) => {
    clientError.value = ''
    try {
      const data = await apiPostJson('/api/clients/unpair', { uuid })
      if (!isSuccessfulStatus(data.status)) throw new Error(data.error || 'Failed to unpair client')
      clearEditingState(uuid)
      await refreshClients()
      return true
    } catch (error) {
      console.error('Failed to unpair client:', error)
      clientError.value = error.message
      return false
    }
  }

  const startEdit = (uuid) => {
    const client = clients.value.find((c) => c.uuid === uuid)
    if (client && !editingStates[uuid]) {
      originalValues[uuid] = { ...client }
    }
    editingStates[uuid] = true
  }

  const cancelEdit = (uuid) => {
    const client = clients.value.find((c) => c.uuid === uuid)
    if (client && originalValues[uuid]) {
      for (const field of EDITABLE_CLIENT_FIELDS) client[field] = originalValues[uuid][field]
    }
    editingStates[uuid] = false
  }

  const saveClient = async (uuid) => {
    if (!config.value) return false

    saving.value = true
    clientError.value = ''
    let renamed = false
    try {
      const tmpClients = parseClients()
      const client = clients.value.find((c) => c.uuid === uuid)
      if (!client) return false

      if (originalValues[uuid] && client.name !== originalValues[uuid].name) {
        const name = client.name
        const rename = await apiPostJson('/api/clients/rename', { uuid, name })
        if (!isSuccessfulStatus(rename.status)) throw new Error(rename.error || 'Client rename failed')
        originalValues[uuid].name = name
        renamed = true
      }

      const index = tmpClients.findIndex((c) => c.uuid === uuid)
      if (index >= 0) {
        tmpClients[index] = persistedClient(client)
      } else {
        tmpClients.push(persistedClient(client))
      }

      const serializedClients = JSON.stringify(tmpClients)
      const data = await apiPostJson('/api/clients/list', { clients: serializedClients })

      if (!isSuccessfulStatus(data.status)) throw new Error(data.error || 'Failed to save client settings')
      config.value.clients = serializedClients
      editingStates[uuid] = false
      originalValues[uuid] = { ...client }
      return true
    } catch (error) {
      console.error('Failed to save client:', error)
      clientError.value = renamed ? translate('connections.rename_partial', { error: error.message }) : error.message
      return false
    } finally {
      saving.value = false
    }
  }

  const pairClient = async (pin, name) => {
    clientError.value = ''
    try {
      const data = await apiPostJson('/api/pin', { pin, name })
      if (!isSuccessfulStatus(data.status)) throw new Error(data.error || 'Pairing failed')
      return true
    } catch (error) {
      clientError.value = error.message
      return false
    }
  }

  const loadConfig = async () => {
    try {
      const data = await getBootstrapConfig()
      config.value = data
      pairingDeviceName.value = data.pair_name ?? ''
    } catch (error) {
      console.error('Failed to load config:', error)
    }
  }

  const loadColorProfiles = async () => {
    try {
      const data = await apiJson('/api/color-profiles')
      if (isSuccessfulStatus(data.status) && data.supported !== false) {
        hasIccFileList.value = true
        hdrProfileList.value = (data.profiles || []).filter(isColorProfileFile)
        return
      }
    } catch (error) {
      console.error('Failed to load color profiles:', error)
    }

    // Older desktop shells expose the same information through their bridge.
    if (globalThis.window?.electron?.getIccFileList) {
      hasIccFileList.value = true
      globalThis.window.electron.getIccFileList((files = []) => {
        hdrProfileList.value = files.filter(isColorProfileFile)
      })
    } else {
      hasIccFileList.value = false
    }
  }

  return {
    pairingDeviceName,
    config,
    clients,
    hdrProfileList,
    hasIccFileList,
    loading,
    saving,
    updating,
    clientError,
    listError,
    setClientEnabled,
    pairClient,
    refreshClients,
    unpairAll,
    unpairSingle,
    saveClient,
    startEdit,
    cancelEdit,
    loadConfig,
    loadColorProfiles,
  }
}
