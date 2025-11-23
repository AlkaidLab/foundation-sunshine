const getStoredTheme = () => localStorage.getItem('theme')
export const setStoredTheme = (theme) => localStorage.setItem('theme', theme)

export const getPreferredTheme = () => {
  const storedTheme = getStoredTheme()
  if (storedTheme) {
    return storedTheme
  }

  return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'
}

export const setTheme = (theme) => {
  if (theme === 'auto') {
    document.documentElement.setAttribute(
      'data-bs-theme',
      window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'
    )
  } else {
    document.documentElement.setAttribute('data-bs-theme', theme)
  }
}

export const showActiveTheme = (theme, focus = false) => {
  const themeSwitcher = document.querySelector('#bd-theme')

  if (!themeSwitcher) {
    return
  }

  const themeSwitcherText = document.querySelector('#bd-theme-text')
  const activeThemeIcon = document.querySelector('.theme-icon-active i')
  const btnToActive = document.querySelector(`[data-bs-theme-value="${theme}"]`)
  
  if (!btnToActive) {
    return
  }
  
  const classListOfActiveBtn = btnToActive.querySelector('i').classList

  document.querySelectorAll('[data-bs-theme-value]').forEach((element) => {
    element.classList.remove('active')
    element.setAttribute('aria-pressed', 'false')
  })

  btnToActive.classList.add('active')
  btnToActive.setAttribute('aria-pressed', 'true')
  activeThemeIcon.classList.remove(...activeThemeIcon.classList.values())
  activeThemeIcon.classList.add(...classListOfActiveBtn)
  const themeSwitcherLabel = `${themeSwitcherText.textContent} (${btnToActive.textContent.trim()})`
  themeSwitcher.setAttribute('aria-label', themeSwitcherLabel)

  if (focus) {
    themeSwitcher.focus()
  }
}

// 单例标志，确保全局事件监听器只添加一次
let isAutoThemeInitialized = false
let mediaQueryHandler = null
let domContentLoadedHandler = null

export function loadAutoTheme() {
  // 设置主题
  setTheme(getPreferredTheme())

  // 只在第一次调用时添加全局事件监听器
  if (!isAutoThemeInitialized) {
    // 处理系统主题变化
    const mediaQuery = window.matchMedia('(prefers-color-scheme: dark)')
    mediaQueryHandler = () => {
    const storedTheme = getStoredTheme()
    if (storedTheme !== 'light' && storedTheme !== 'dark') {
      setTheme(getPreferredTheme())
    }
    }
    mediaQuery.addEventListener('change', mediaQueryHandler)

    // 处理 DOMContentLoaded 事件（如果文档已经加载完成，则立即执行）
    domContentLoadedHandler = () => {
    showActiveTheme(getPreferredTheme())
    }
    if (document.readyState === 'loading') {
      window.addEventListener('DOMContentLoaded', domContentLoadedHandler)
    } else {
      // 文档已经加载完成，直接执行
      domContentLoadedHandler()
    }

    isAutoThemeInitialized = true
  }
}
