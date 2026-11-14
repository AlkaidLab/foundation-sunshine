import { ref, reactive, computed } from 'vue'
import { apiPostJson } from '../utils/apiFetch.js'

/**
 * 向主机提交新的 Web UI 账号。返回 `{ ok }`，失败时 `error` 是主机的消息或翻译键。
 * 欢迎页和新手引导共用。
 */
export async function postCredentials(data) {
  try {
    const result = await apiPostJson('/api/password', data)
    if (result.status?.toString() === 'true') return { ok: true }
    // 如果服务器返回了错误消息，使用它；否则使用翻译键
    return { ok: false, error: result.error || 'welcome.server_error' }
  } catch (err) {
    console.error('Failed to save password:', err)
    return { ok: false, error: 'welcome.network_error' }
  }
}

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

    const result = await postCredentials(passwordData)
    if (result.ok) {
      success.value = true
      setTimeout(() => {
        window.location.href = '/'
      }, 2000)
    } else {
      error.value = result.error
    }
    loading.value = false
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
