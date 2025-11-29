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
      error.value = '密码不匹配'
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
        error.value = result.error || `服务器错误: ${response.status}`
      }
    } catch (err) {
      console.error('保存密码失败:', err)
      error.value = '网络错误，请检查连接'
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
