import { onMounted } from 'vue'
import { loadAutoTheme, showActiveTheme, getPreferredTheme } from '../utils/theme.js'

export function useTheme() {
  onMounted(() => {
    loadAutoTheme()
    showActiveTheme(getPreferredTheme(), false)
  })

  return {
    // 可以在这里暴露更多主题相关的功能
  }
}
