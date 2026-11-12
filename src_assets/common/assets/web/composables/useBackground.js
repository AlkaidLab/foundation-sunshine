import { useNotify } from 'foundation-ui/notify'
import { getCurrentScope, onScopeDispose } from 'vue'
import { translateFallback } from '../utils/localizedMessage.js'

// The default picture is the shared appearance's (foundation-ui); a picture the person dropped here is kept under this key.
const DEFAULT_BACKGROUND = ''
const STORAGE_KEY = 'customBackground'
const THUMBNAIL_MAX_SIZE = 200
const TEXT_COLOR_PROPERTIES = [
  '--text-primary-color',
  '--text-secondary-color',
  '--text-muted-color',
  '--text-title-color',
]

// The wallpaper is the shared appearance of the window (foundation-ui: 薇薇's pictures by default, the person's file or address, a gradient or a
// plain colour). A picture dropped on this page, or stored by an older version, becomes the person's file there; nothing else draws a wallpaper.
// loaded on demand: the shared package pulls in the whole component set
const appearance = async () => (await import('foundation-ui/vue')).useAppearance()
const setWallpaper = async (imageUrl) => {
  if (imageUrl) await (await appearance()).chooseDataUrl(imageUrl)
}

const resetTextColorTheme = () => {
  const root = document.documentElement
  TEXT_COLOR_PROPERTIES.forEach((property) => root.style.removeProperty(property))
  root.classList.remove('text-color-transitioning')
  document.body.classList.remove('bg-light', 'bg-dark')
}

/**
 * 背景图片管理组合式函数
 */
export function useBackground(options = {}) {
  const {
    defaultBackground = DEFAULT_BACKGROUND,
    storageKey = STORAGE_KEY,
    maxWidth = 1920,
    maxHeight = 1080,
    maxSizeMB = 2,
    translate = translateFallback,
  } = options

  const getCurrentBackground = () => localStorage.getItem(storageKey) ?? defaultBackground

  const isDevMode = () => localStorage.getItem('sunshine_dev_mode') === '1'

  const clearDevBypass = () => {
    if (localStorage.getItem('sunshine_dev_mode') === '1') {
      localStorage.setItem('sunshine_dev_mode', '0')
    }
  }

  const setBackground = async (imageUrl) => {
    resetTextColorTheme()
    // developer mode (seven taps) shows the plain page
    if (isDevMode()) { await (await appearance()).set({ background: 'solid' }); return }
    // the picture moves to the shared appearance once; from then on the personalise sheet decides which one shows
    if (imageUrl) { await setWallpaper(imageUrl); try { localStorage.removeItem(storageKey) } catch { /* storage unavailable */ } }
  }

  const recheckBackgroundBrightness = async () => {
    await setBackground(getCurrentBackground())
  }

  const loadBackground = () => setBackground(getCurrentBackground())

  const saveBackground = async (imageData) => {
    clearDevBypass()
    try {
      localStorage.setItem(storageKey, imageData)
    } catch (error) {
      if (error.name === 'QuotaExceededError') {
        localStorage.removeItem(storageKey)
        try {
          localStorage.setItem(storageKey, imageData)
        } catch {
          throw new Error(translate('apps.feedback.image_too_large'))
        }
      } else {
        throw error
      }
    }
    await setBackground(imageData)
  }

  const calculateResizedDimensions = (width, height) => {
    if (width <= maxWidth && height <= maxHeight) return { width, height }
    const ratio = Math.min(maxWidth / width, maxHeight / height)
    return { width: width * ratio, height: height * ratio }
  }

  const compressWithQuality = (img, width, height, quality) => {
    const canvas = document.createElement('canvas')
    canvas.width = width
    canvas.height = height
    canvas.getContext('2d').drawImage(img, 0, 0, width, height)

    const dataUrl = canvas.toDataURL('image/jpeg', quality)
    const sizeInMB = (dataUrl.length * 3) / 4 / 1024 / 1024

    if (sizeInMB <= maxSizeMB) return dataUrl
    if (quality > 0.3) return compressWithQuality(img, width, height, quality - 0.1)
    return null
  }

  const compressImage = (file, initialQuality = 0.8) =>
    new Promise((resolve, reject) => {
      const reader = new FileReader()
      reader.onload = (event) => {
        const img = new Image()
        img.onload = () => {
          const { width, height } = calculateResizedDimensions(img.width, img.height)
          const result = compressWithQuality(img, width, height, initialQuality)
          result ? resolve(result) : reject(new Error(translate('apps.feedback.image_too_large')))
        }
        img.onerror = () => reject(new Error(translate('apps.image_load_failed')))
        img.src = event.target.result
      }
      reader.onerror = () => reject(new Error(translate('apps.feedback.file_read_failed')))
      reader.readAsDataURL(file)
    })

  const handleDragOver = (e) => {
    e.preventDefault()
    if (e.dataTransfer?.types?.includes('Files')) {
      document.body.classList.add('dragover')
    }
  }

  const handleDragLeave = () => document.body.classList.remove('dragover')

  const handleDrop = async (e, onError) => {
    e.preventDefault()
    document.body.classList.remove('dragover')

    const file = e.dataTransfer?.files?.[0]
    if (!file?.type.startsWith('image/')) return

    try {
      await saveBackground(await compressImage(file))
    } catch (error) {
      onError?.(error) ?? useNotify().snack(error.message || translate('apps.feedback.image_process_failed'))
    }
  }

  const addDragListeners = (onError) => {
    const handlers = {
      dragover: handleDragOver,
      dragleave: handleDragLeave,
      drop: (e) => handleDrop(e, onError),
    }

    Object.entries(handlers).forEach(([event, handler]) => document.addEventListener(event, handler))
    return () => Object.entries(handlers).forEach(([event, handler]) => document.removeEventListener(event, handler))
  }

  const clearBackground = () => {
    clearDevBypass()
    localStorage.removeItem(storageKey)
    return setBackground(defaultBackground)
  }

  // 监听主题切换
  if (typeof document !== 'undefined') {
    const handleThemeChange = () => setTimeout(recheckBackgroundBrightness, 100)
    const observerConfig = { attributes: true, attributeFilter: ['data-bs-theme'] }
    const observer = new MutationObserver(handleThemeChange)
    observer.observe(document.documentElement, observerConfig)
    observer.observe(document.body, observerConfig)

    if (getCurrentScope()) {
      onScopeDispose(() => observer.disconnect())
    }

    // 监听背景旁路切换
    const onBackgroundBypass = (e) => {
      if (e.detail?.enabled) {
        document.body.style.background = ''
      } else {
        loadBackground()
      }
    }
    window.addEventListener('sunshine-background-bypass', onBackgroundBypass)
    if (getCurrentScope()) {
      onScopeDispose(() => window.removeEventListener('sunshine-background-bypass', onBackgroundBypass))
    }
  }

  return {
    setBackground,
    loadBackground,
    saveBackground,
    compressImage,
    handleDragOver,
    handleDragLeave,
    handleDrop,
    addDragListeners,
    clearBackground,
    getCurrentBackground,
    recheckBackgroundBrightness,
  }
}
