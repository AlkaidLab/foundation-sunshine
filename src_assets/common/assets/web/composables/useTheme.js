import { onMounted, onUnmounted } from 'vue'
import { 
  loadAutoTheme, 
  setupThemeToggleListener, 
  removeThemeToggleListener 
} from '../utils/theme.js'

/**
 * 主题切换组合式函数
 * 提供主题相关的功能
 */
export function useTheme() {
  onMounted(() => {
    loadAutoTheme()
    setupThemeToggleListener()
  })

  onUnmounted(() => {
    removeThemeToggleListener()
  })

  return {
    // 可以在这里暴露更多主题相关的功能
  }
}

