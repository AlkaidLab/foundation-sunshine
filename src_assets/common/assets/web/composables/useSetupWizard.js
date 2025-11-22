import { ref } from 'vue'

/**
 * 设置向导组合式函数
 */
export function useSetupWizard() {
  const showSetupWizard = ref(false)
  const adapters = ref([])
  const displayDevices = ref([])
  const hasLocale = ref(false)

  // 检查是否需要显示设置向导
  const checkSetupWizard = (config) => {
    const isFirstTime = config.setup_wizard_completed == true || 
                       config.setup_wizard_completed == 'true'
    
    if (!isFirstTime) {
      showSetupWizard.value = true
      adapters.value = config.adapters || []
      displayDevices.value = config.display_devices || []
      hasLocale.value = !!(config.locale && config.locale !== '')
      return true
    }
    return false
  }

  // 设置完成回调
  const onSetupComplete = (config) => {
    console.log('设置完成:', config)
    // 用户点击"配置应用程序"按钮后会自动跳转到 /apps
  }

  return {
    showSetupWizard,
    adapters,
    displayDevices,
    hasLocale,
    checkSetupWizard,
    onSetupComplete,
  }
}

