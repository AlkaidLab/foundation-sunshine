import { ref, reactive } from 'vue'

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
  // 为每个客户端维护独立的编辑状态和原始值
  const editingStates = reactive({})
  const originalValues = reactive({})

  // 初始化客户端编辑状态
  const initClientEditingState = (client) => {
    if (!editingStates[client.uuid]) {
      editingStates[client.uuid] = false
      originalValues[client.uuid] = { ...client }
    }
  }

  // 刷新客户端列表
  const refreshClients = async () => {
    loading.value = true
    try {
      const response = await fetch('/api/clients/list')
      const data = await response.json()

      if (data.status === 'true' && data.named_certs && data.named_certs.length) {
        clients.value = data.named_certs
      }

      let tmpClients = []
      try {
        tmpClients = JSON.parse(config.value?.clients || '[]')
      } catch (error) {
        console.error('Failed to parse clients:', error)
      }

      clients.value = clients.value.map((client) => {
        const merged = {
          ...client,
          ...tmpClients.find(({ uuid }) => uuid === client.uuid),
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

  // 取消所有配对
  const unpairAll = async () => {
    unpairAllPressed.value = true
    try {
      const response = await fetch('/api/clients/unpair-all', { method: 'POST' })
      const data = await response.json()
      showApplyMessage.value = true
      unpairAllStatus.value = data.status.toString() === 'true'

      if (unpairAllStatus.value) {
        // 清理所有编辑状态
        Object.keys(editingStates).forEach((uuid) => {
          delete editingStates[uuid]
          delete originalValues[uuid]
        })
        await refreshClients()
      }

      setTimeout(() => {
        unpairAllStatus.value = null
      }, 5000)
    } catch (error) {
      console.error('Failed to unpair all:', error)
      unpairAllStatus.value = false
    } finally {
      unpairAllPressed.value = false
    }
  }

  // 取消单个配对
  const unpairSingle = async (uuid) => {
    deleting.value.add(uuid)
    try {
      const response = await fetch('/api/clients/unpair', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({ uuid }),
      })
      const data = await response.json()
      const status = data.status?.toString().toLowerCase()
      const isSuccess = status === '1' || status === 'true'
      if (isSuccess) {
        showApplyMessage.value = true
        // 清理编辑状态
        delete editingStates[uuid]
        delete originalValues[uuid]
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

  // 进入编辑模式
  const startEdit = (uuid) => {
    if (!originalValues[uuid]) {
      const client = clients.value.find((c) => c.uuid === uuid)
      if (client) {
        originalValues[uuid] = { ...client }
      }
    }
    editingStates[uuid] = true
  }

  // 取消编辑
  const cancelEdit = (uuid) => {
    if (originalValues[uuid]) {
      const client = clients.value.find((c) => c.uuid === uuid)
      if (client) {
        Object.assign(client, originalValues[uuid])
      }
    }
    editingStates[uuid] = false
  }

  // 保存单个客户端配置
  const saveClient = async (uuid) => {
    if (!config.value) return false

    saving.value = true
    try {
      // 更新配置中的客户端信息
      let tmpClients = []
      try {
        tmpClients = JSON.parse(config.value.clients || '[]')
      } catch (error) {
        tmpClients = []
      }

      const client = clients.value.find((c) => c.uuid === uuid)
      if (!client) return false

      const index = tmpClients.findIndex((c) => c.uuid === uuid)
      if (index >= 0) {
        tmpClients[index] = { ...client }
      } else {
        tmpClients.push({ ...client })
      }

      config.value.clients = serialize(tmpClients)

      const response = await fetch('/api/clients/list', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(config.value),
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

  // 保存所有客户端配置（保留向后兼容）
  const save = async () => {
    if (!config.value) return false

    saving.value = true
    try {
      config.value.clients = serialize(clients.value)
      const response = await fetch('/api/clients/list', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(config.value),
      })

      if (response.status === 200) {
        // 更新所有编辑状态
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

  // 检查客户端是否有未保存的更改
  const hasUnsavedChanges = (uuid) => {
    const client = clients.value.find((c) => c.uuid === uuid)
    const original = originalValues[uuid]
    if (!client || !original) return false
    return client.hdrProfile !== original.hdrProfile
  }

  // 序列化客户端列表
  const serialize = (listArray = []) => {
    const nl = '\n'
    return '[' + nl + '    ' + listArray.map((item) => JSON.stringify(item)).join(',' + nl + '    ') + nl + ']'
  }

  // 初始化 PIN 表单
  const initPinForm = (onSuccess) => {
    const form = document.querySelector('#form')
    if (!form) return

    form.addEventListener('submit', async (e) => {
      e.preventDefault()
      const pinInput = document.querySelector('#pin-input')
      const nameInput = document.querySelector('#name-input')
      const statusDiv = document.querySelector('#status')

      if (!pinInput || !nameInput || !statusDiv) return

      const pin = pinInput.value
      const name = nameInput.value
      statusDiv.innerHTML = ''

      try {
        const response = await fetch('/api/pin', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json',
          },
          body: JSON.stringify({ pin, name }),
        })

        const data = await response.json()

        if (data.status.toString().toLowerCase() === 'true') {
          statusDiv.innerHTML =
            '<div class="alert alert-success" role="alert">Success! Please check Moonlight to continue</div>'
          pinInput.value = ''
          nameInput.value = ''
          if (onSuccess) onSuccess()
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

  // 点击应用横幅
  const clickedApplyBanner = async () => {
    showApplyMessage.value = false
    try {
      await fetch('/api/restart', { method: 'POST' })
    } catch (error) {
      console.error('Failed to restart:', error)
    }
  }

  // 加载配置
  const loadConfig = async () => {
    try {
      const response = await fetch('/api/config')
      const data = await response.json()
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
