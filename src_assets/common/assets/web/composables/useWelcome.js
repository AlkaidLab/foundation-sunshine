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

  // 密码是否匹配
  const passwordsMatch = computed(() => {
    if (!passwordData.newPassword || !passwordData.confirmNewPassword) {
      return true
    }
    return passwordData.newPassword === passwordData.confirmNewPassword
  })

  // 表单是否有效
  const isFormValid = computed(() => {
    return (
      passwordData.newUsername &&
      passwordData.newPassword &&
      passwordData.confirmNewPassword &&
      passwordsMatch.value
    )
  })

  // 保存密码
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
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(passwordData),
      })

      if (response.status === 200) {
        const result = await response.json()

        if (result.status?.toString() === 'true') {
          success.value = true
          setTimeout(() => {
            window.location.href = '/'
          }, 2000)
        } else {
          error.value = result.error || '未知错误'
        }
      } else {
        error.value = `服务器错误: ${response.status}`
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

