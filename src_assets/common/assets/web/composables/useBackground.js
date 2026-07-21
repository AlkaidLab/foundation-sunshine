import { getCurrentScope, onScopeDispose } from 'vue'
import { extractColors, rgbToHsl as rgbToHslValues, selectAccentColor } from '../utils/colorPalette.js'

const DEFAULT_BACKGROUND = 'https://assets.alkaidlab.com/sunshine-bg0.webp'
const STORAGE_KEY = 'customBackground'
const THUMBNAIL_MAX_SIZE = 200
const TEXT_COLOR_PROPERTIES = [
  '--text-primary-color',
  '--text-secondary-color',
  '--text-muted-color',
  '--text-title-color',
]

const COLOR_CONFIG = {
  textLightnessRange: { min: 15, max: 95 },
  brightnessThreshold: 50,
  paletteSize: 6,
}

const createDefaultColorInfo = () => ({
  dominantColor: { r: 128, g: 128, b: 128 },
  hsl: { h: 0, s: 0, l: 50 },
  palette: [],
})

const clamp = (value, min, max) => Math.max(min, Math.min(max, value))

const isLocalImage = (imageUrl) =>
  typeof imageUrl === 'string' && (imageUrl.startsWith('data:') || imageUrl.startsWith('blob:'))

const loadImage = (imageUrl) =>
  new Promise((resolve, reject) => {
    const img = new Image()
    img.onload = () => resolve(img)
    img.onerror = () => reject(new Error('图片加载失败'))
    img.src = imageUrl
  })

const rgbToRoundedHsl = (r, g, b) => {
  const [h, s, l] = rgbToHslValues(r, g, b)
  return { h: Math.round(h), s: Math.round(s), l: Math.round(l) }
}

const hslToRgb = (h, s, l) => {
  h /= 360
  s /= 100
  l /= 100

  if (s === 0) {
    const val = Math.round(l * 255)
    return { r: val, g: val, b: val }
  }

  const hue2rgb = (p, q, t) => {
    if (t < 0) t += 1
    if (t > 1) t -= 1
    if (t < 1 / 6) return p + (q - p) * 6 * t
    if (t < 1 / 2) return q
    if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6
    return p
  }

  const q = l < 0.5 ? l * (1 + s) : l + s - l * s
  const p = 2 * l - q

  return {
    r: Math.round(hue2rgb(p, q, h + 1 / 3) * 255),
    g: Math.round(hue2rgb(p, q, h) * 255),
    b: Math.round(hue2rgb(p, q, h - 1 / 3) * 255),
  }
}

const rgbToHex = (r, g, b) => {
  const toHex = (x) => Math.round(x).toString(16).padStart(2, '0')
  return `#${toHex(r)}${toHex(g)}${toHex(b)}`
}

const analyzeImageColors = (img) => {
  if (!img.complete || !img.width || !img.height) return createDefaultColorInfo()

  try {
    const canvas = document.createElement('canvas')
    const scale = Math.min(1, THUMBNAIL_MAX_SIZE / Math.max(img.width, img.height))
    canvas.width = Math.max(1, Math.round(img.width * scale))
    canvas.height = Math.max(1, Math.round(img.height * scale))

    const context = canvas.getContext('2d', { willReadFrequently: true })
    if (!context) return createDefaultColorInfo()

    context.drawImage(img, 0, 0, canvas.width, canvas.height)
    const palette = extractColors(context.getImageData(0, 0, canvas.width, canvas.height), COLOR_CONFIG.paletteSize)
    const accentColor = selectAccentColor(palette)
    if (!accentColor) return createDefaultColorInfo()

    const [r, g, b] = accentColor
    return {
      dominantColor: { r, g, b },
      hsl: rgbToRoundedHsl(r, g, b),
      palette,
    }
  } catch {
    return createDefaultColorInfo()
  }
}

const detectImageColorInfo = async (imageUrl) => {
  try {
    const img = await loadImage(imageUrl)
    return analyzeImageColors(img)
  } catch {
    return createDefaultColorInfo()
  }
}

const calculateTextColors = (colorInfo) => {
  const { hsl } = colorInfo
  const { textLightnessRange, brightnessThreshold } = COLOR_CONFIG
  const isLightBg = hsl.l > brightnessThreshold

  const textH = hsl.h
  let textS = hsl.s
  let textL

  if (isLightBg) {
    textL = Math.max(textLightnessRange.min, hsl.l - 60)
    if (hsl.s < 30) textS = Math.min(20, hsl.s)
  } else {
    textL = Math.min(textLightnessRange.max, hsl.l + 70)
    textS = Math.min(40, hsl.s * 0.6)
  }

  const createColor = (s, l) => {
    const rgb = hslToRgb(textH, s, l)
    return rgbToHex(rgb.r, rgb.g, rgb.b)
  }

  return {
    primary: createColor(textS, textL),
    secondary: createColor(textS * 0.7, clamp(isLightBg ? textL - 15 : textL + 10, 10, 90)),
    muted: createColor(textS * 0.4, clamp(isLightBg ? textL - 25 : textL + 20, 5, 85)),
    title: createColor(textS * 0.3, isLightBg ? textLightnessRange.min : textLightnessRange.max),
    bgClass: isLightBg ? 'bg-light' : 'bg-dark',
  }
}

const setTextColorTheme = (colorInfo) => {
  const root = document.documentElement
  const textColors = calculateTextColors(colorInfo)

  root.style.setProperty('--text-primary-color', textColors.primary)
  root.style.setProperty('--text-secondary-color', textColors.secondary)
  root.style.setProperty('--text-muted-color', textColors.muted)
  root.style.setProperty('--text-title-color', textColors.title)

  document.body.classList.remove('bg-light', 'bg-dark')
  document.body.classList.add(textColors.bgClass)

  root.classList.add('text-color-transitioning')
  setTimeout(() => root.classList.remove('text-color-transitioning'), 500)
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
  } = options

  const getCurrentBackground = () => localStorage.getItem(storageKey) ?? defaultBackground

  const isDevMode = () => localStorage.getItem('sunshine_dev_mode') === '1'

  const clearDevBypass = () => {
    if (localStorage.getItem('sunshine_dev_mode') === '1') {
      localStorage.setItem('sunshine_dev_mode', '0')
    }
  }

  let colorUpdateId = 0

  const refreshTextColorTheme = async (imageUrl) => {
    const updateId = ++colorUpdateId
    if (!isLocalImage(imageUrl)) {
      resetTextColorTheme()
      return
    }

    try {
      const colorInfo = await detectImageColorInfo(imageUrl)
      if (updateId === colorUpdateId) setTextColorTheme(colorInfo)
    } catch {
      // 静默失败
    }
  }

  const setBackground = async (imageUrl) => {
    if (isDevMode()) {
      colorUpdateId++
      document.body.style.background = ''
      resetTextColorTheme()
      return
    }
    document.body.style.background = `url(${imageUrl}) center/cover fixed no-repeat`
    await refreshTextColorTheme(imageUrl)
  }

  const recheckBackgroundBrightness = async () => {
    await refreshTextColorTheme(getCurrentBackground())
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
          throw new Error('图片太大，无法存储。请选择更小的图片或降低图片质量。')
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
          result ? resolve(result) : reject(new Error('图片太大，无法存储。请选择更小的图片。'))
        }
        img.onerror = () => reject(new Error('图片加载失败'))
        img.src = event.target.result
      }
      reader.onerror = () => reject(new Error('文件读取失败'))
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
      onError?.(error) ?? alert(error.message || '处理图片时发生错误')
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
