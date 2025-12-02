import { ref, reactive, computed } from 'vue'

/**
 * 欢迎页面组合式函数
 */
export function useWelcome() {
  const error = ref(null)
  const success = ref(false)
  const loading = ref(false)

  const passwordData = reactive({
    newUsername: 'sunshine',
    newPassword: '',
    confirmNewPassword: '',
  })

  const passwordsMatch = computed(
    () =>
      !passwordData.newPassword ||
      !passwordData.confirmNewPassword ||
      passwordData.newPassword === passwordData.confirmNewPassword
  )

  const isFormValid = computed(
    () =>
      passwordData.newUsername && passwordData.newPassword && passwordData.confirmNewPassword && passwordsMatch.value
  )

  const save = async () => {
    error.value = null

    if (!passwordsMatch.value) {
      error.value = 'welcome.password_mismatch'
      return
    }

    loading.value = true

    try {
      const response = await fetch('/api/password', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(passwordData),
      })

      const result = await response.json()

      if (response.ok && result.status?.toString() === 'true') {
        success.value = true
        setTimeout(() => {
          window.location.href = '/'
        }, 2000)
      } else {
        // 如果服务器返回了错误消息，使用它；否则使用翻译键
        error.value = result.error || 'welcome.server_error'
      }
    } catch (err) {
      console.error('Failed to save password:', err)
      error.value = 'welcome.network_error'
    } finally {
      loading.value = false
    }
  }

  return {
    error,
    success,
    loading,
    passwordData,
    passwordsMatch,
    isFormValid,
    save,
  }
}
