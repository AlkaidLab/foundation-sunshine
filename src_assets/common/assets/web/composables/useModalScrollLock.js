import { ref, watch, onBeforeUnmount, unref } from 'vue'

/**
 * 模态框滚动锁定 Composable
 * 用于在模态框打开时锁定背景页面滚动，关闭时恢复滚动位置
 *
 * @param {import('vue').Ref<boolean>|() => boolean} isOpen - 模态框是否打开（响应式引用或返回布尔值的函数）
 * @param {Object} options - 配置选项
 * @param {boolean} options.scrollToTop - 是否在打开时滚动到顶部（默认: true）
 * @param {boolean} options.restoreScroll - 是否在关闭时恢复滚动位置（默认: true）
 * @returns {Object} 返回锁定和恢复方法
 */
export function useModalScrollLock(isOpen, options = {}) {
  const { scrollToTop = true, restoreScroll = true } = options
  const savedScrollPosition = ref(0)
  const isLocked = ref(false)

  /**
   * 获取滚动条宽度
   */
  const getScrollbarWidth = () => window.innerWidth - document.documentElement.clientWidth

  /**
   * 锁定 body 滚动
   */
  const lockBodyScroll = () => {
    if (isLocked.value) return

    const { body } = document
    const scrollbarWidth = getScrollbarWidth()

    // 保存当前滚动位置
    savedScrollPosition.value = window.pageYOffset || document.documentElement.scrollTop

    // 锁定滚动并补偿滚动条宽度
    body.style.overflow = 'hidden'
    if (scrollbarWidth > 0) {
      body.style.paddingRight = `${scrollbarWidth}px`
    }

    // 滚动到顶部
    if (scrollToTop) {
      window.scrollTo({ top: 0, behavior: 'instant' })
    }

    isLocked.value = true
  }

  /**
   * 恢复 body 滚动
   */
  const restoreBodyScroll = () => {
    if (!isLocked.value) return

    const { body } = document

    // 恢复 body 样式
    body.style.overflow = ''
    body.style.paddingRight = ''

    // 恢复滚动位置
    if (restoreScroll && savedScrollPosition.value > 0) {
      requestAnimationFrame(() => {
        window.scrollTo({ top: savedScrollPosition.value, behavior: 'smooth' })
        savedScrollPosition.value = 0
      })
    } else {
      savedScrollPosition.value = 0
    }

    isLocked.value = false
  }

  // 监听 isOpen 变化
  watch(
    () => (typeof isOpen === 'function' ? isOpen() : unref(isOpen)),
    (newVal) => {
      newVal ? lockBodyScroll() : restoreBodyScroll()
    },
    { immediate: true }
  )

  // 组件卸载时确保恢复滚动
  onBeforeUnmount(restoreBodyScroll)

  return {
    lockBodyScroll,
    restoreBodyScroll,
    isLocked,
  }
}
