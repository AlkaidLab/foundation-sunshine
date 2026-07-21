import { ref, reactive } from 'vue'
import { getBootstrapConfig } from '../config/bootstrapData.js'
import { apiFetch, apiJson, apiPostJson } from '../utils/apiFetch.js'

const STATUS_RESET_DELAY = 5000

/**
 * PIN 配对组合式函数
 */
export function usePin() {
  const pairingDeviceName = ref('')
  const unpairAllPressed = ref(false)
  const unpairAllStatus = ref(null)
  const showApplyMessage = ref(false)
  const config = ref(null)
  const clients = ref([])
  const hdrProfileList = ref([])
  const hasIccFileList = ref(false)
  const loading = ref(false)
  const saving = ref(false)
  const deleting = ref(new Set())
  const editingStates = reactive({})
  const originalValues = reactive({})

  const initClientEditingState = (client) => {
    if (!editingStates[client.uuid]) {
      editingStates[client.uuid] = false
      originalValues[client.uuid] = { ...client }
    }
  }

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

  const serialize = (listArray = []) => {
    const nl = '\n'
    return '[' + nl + '    ' + listArray.map((item) => JSON.stringify(item)).join(',' + nl + '    ') + nl + ']'
  }

  const refreshClients = async () => {
    loading.value = true
    try {
      const data = await apiJson('/api/clients/list')

      if (data.status === 'true' && data.named_certs?.length) {
        clients.value = data.named_certs
      }

      const tmpClients = parseClients()
      clients.value = clients.value.map((client) => {
        const merged = { ...client, ...tmpClients.find(({ uuid }) => uuid === client.uuid) }
        // 如果客户端没有deviceSize，设置默认值为medium
        if (!merged.deviceSize) {
          merged.deviceSize = 'medium'
        }
        initClientEditingState(merged)
        return merged
      })
    } catch (error) {
      console.error('Failed to refresh clients:', error)
    } finally {
      loading.value = false
    }
  }

  const unpairAll = async () => {
    unpairAllPressed.value = true
    try {
      const data = await apiJson('/api/clients/unpair-all', { method: 'POST' })
      showApplyMessage.value = true
      unpairAllStatus.value = data.status.toString() === 'true'

      if (unpairAllStatus.value) {
        clearAllEditingStates()
        await refreshClients()
      }

      setTimeout(() => {
        unpairAllStatus.value = null
      }, STATUS_RESET_DELAY)
    } catch (error) {
      console.error('Failed to unpair all:', error)
      unpairAllStatus.value = false
    } finally {
      unpairAllPressed.value = false
    }
  }

  const unpairSingle = async (uuid) => {
    deleting.value.add(uuid)
    try {
      const data = await apiPostJson('/api/clients/unpair', { uuid })
      const status = data.status?.toString().toLowerCase()
      if (status === '1' || status === 'true') {
        showApplyMessage.value = true
        clearEditingState(uuid)
        await refreshClients()
        return true
      }
      return false
    } catch (error) {
      console.error('Failed to unpair client:', error)
      return false
    } finally {
      deleting.value.delete(uuid)
    }
  }

  const startEdit = (uuid) => {
    const client = clients.value.find((c) => c.uuid === uuid)
    if (client && !originalValues[uuid]) {
      originalValues[uuid] = { ...client }
    }
    editingStates[uuid] = true
  }

  const cancelEdit = (uuid) => {
    const client = clients.value.find((c) => c.uuid === uuid)
    if (client && originalValues[uuid]) {
      Object.assign(client, originalValues[uuid])
    }
    editingStates[uuid] = false
  }

  const saveClient = async (uuid) => {
    if (!config.value) return false

    saving.value = true
    try {
      const tmpClients = parseClients()
      const client = clients.value.find((c) => c.uuid === uuid)
      if (!client) return false

      const index = tmpClients.findIndex((c) => c.uuid === uuid)
      if (index >= 0) {
        tmpClients[index] = { ...client }
      } else {
        tmpClients.push({ ...client })
      }

      config.value.clients = serialize(tmpClients)
      const response = await apiFetch('/api/clients/list', {
        method: 'POST',
        body: config.value,
      })

      if (response.status === 200) {
        editingStates[uuid] = false
        originalValues[uuid] = { ...client }
        return true
      }
      return false
    } catch (error) {
      console.error('Failed to save client:', error)
      return false
    } finally {
      saving.value = false
    }
  }

  const save = async () => {
    if (!config.value) return false

    saving.value = true
    try {
      config.value.clients = serialize(clients.value)
      const response = await apiFetch('/api/clients/list', {
        method: 'POST',
        body: config.value,
      })

      if (response.status === 200) {
        clients.value.forEach((client) => {
          editingStates[client.uuid] = false
          originalValues[client.uuid] = { ...client }
        })
        return true
      }
      return false
    } catch (error) {
      console.error('Failed to save clients:', error)
      return false
    } finally {
      saving.value = false
    }
  }

  const hasUnsavedChanges = (uuid) => {
    const client = clients.value.find((c) => c.uuid === uuid)
    const original = originalValues[uuid]
    return (
      client && original && (client.hdrProfile !== original.hdrProfile || client.deviceSize !== original.deviceSize)
    )
  }

  const initPinForm = (onSuccess) => {
    const form = document.querySelector('#form')
    if (!form) return

    form.addEventListener('submit', async (e) => {
      e.preventDefault()
      const pinInput = document.querySelector('#pin-input')
      const nameInput = document.querySelector('#name-input')
      const statusDiv = document.querySelector('#status')

      if (!pinInput || !nameInput || !statusDiv) return

      statusDiv.innerHTML = ''

      try {
        const data = await apiPostJson('/api/pin', { pin: pinInput.value, name: nameInput.value })

        if (data.status.toString().toLowerCase() === 'true') {
          statusDiv.innerHTML =
            '<div class="alert alert-success" role="alert">Success! Please check Moonlight to continue</div>'
          pinInput.value = ''
          nameInput.value = ''
          onSuccess?.()
        } else {
          statusDiv.innerHTML =
            '<div class="alert alert-danger" role="alert">Pairing Failed: Check if the PIN is typed correctly</div>'
        }
      } catch (error) {
        console.error('Pairing failed:', error)
        statusDiv.innerHTML = '<div class="alert alert-danger" role="alert">Pairing Failed: Network error</div>'
      }
    })
  }

  const clickedApplyBanner = async () => {
    showApplyMessage.value = false
    try {
      await apiFetch('/api/restart', { method: 'POST' })
    } catch (error) {
      console.error('Failed to restart:', error)
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

  return {
    pairingDeviceName,
    unpairAllPressed,
    unpairAllStatus,
    showApplyMessage,
    config,
    clients,
    hdrProfileList,
    hasIccFileList,
    loading,
    saving,
    deleting,
    editingStates,
    originalValues,
    refreshClients,
    unpairAll,
    unpairSingle,
    save,
    saveClient,
    startEdit,
    cancelEdit,
    hasUnsavedChanges,
    serialize,
    initPinForm,
    clickedApplyBanner,
    loadConfig,
  }
}
