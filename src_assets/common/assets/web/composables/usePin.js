import { ref } from 'vue'

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
  const saved = ref(true)

  // 刷新客户端列表
  const refreshClients = async () => {
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

      clients.value = clients.value.map((client) => ({
        ...client,
        ...tmpClients.find(({ uuid }) => uuid === client.uuid),
      }))
    } catch (error) {
      console.error('Failed to refresh clients:', error)
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
    try {
      await fetch('/api/clients/unpair', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({ uuid }),
      })
      showApplyMessage.value = true
      await refreshClients()
    } catch (error) {
      console.error('Failed to unpair client:', error)
    }
  }

  // 保存客户端配置
  const save = async () => {
    if (!config.value) return false
    
    config.value.clients = serialize(clients.value)
    try {
      const response = await fetch('/api/clients/list', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(config.value),
      })
      
      if (response.status === 200) {
        saved.value = true
        return true
      }
      return false
    } catch (error) {
      console.error('Failed to save clients:', error)
      return false
    }
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
          statusDiv.innerHTML = '<div class="alert alert-success" role="alert">Success! Please check Moonlight to continue</div>'
          pinInput.value = ''
          nameInput.value = ''
          if (onSuccess) onSuccess()
        } else {
          statusDiv.innerHTML = '<div class="alert alert-danger" role="alert">Pairing Failed: Check if the PIN is typed correctly</div>'
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
    saved,
    refreshClients,
    unpairAll,
    unpairSingle,
    save,
    serialize,
    initPinForm,
    clickedApplyBanner,
    loadConfig,
  }
}

