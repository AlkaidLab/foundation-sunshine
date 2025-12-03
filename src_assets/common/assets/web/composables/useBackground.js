import ColorThief from 'colorthief'

const DEFAULT_BACKGROUND = 'https://raw.gitmirror.com/qiin2333/qiin.github.io/assets/img/sunshine-bg0.webp'
const STORAGE_KEY = 'customBackground'

const COLOR_CONFIG = {
  textLightnessRange: { min: 15, max: 95 },
  brightnessThreshold: 50,
  paletteSize: 5,
}

const DEFAULT_COLOR_INFO = {
  dominantColor: { r: 128, g: 128, b: 128 },
  hsl: { h: 0, s: 0, l: 50 },
}

const clamp = (value, min, max) => Math.max(min, Math.min(max, value))

const isLocalImage = (imageUrl) => imageUrl.startsWith('data:') || imageUrl.startsWith('blob:')

const loadImage = (imageUrl) =>
  new Promise((resolve, reject) => {
    const img = new Image()
    img.onload = () => resolve(img)
    img.onerror = () => reject(new Error('图片加载失败'))
    img.src = imageUrl
  })

const rgbToHsl = (r, g, b) => {
  r /= 255
  g /= 255
  b /= 255

  const max = Math.max(r, g, b)
  const min = Math.min(r, g, b)
  const l = (max + min) / 2
  const d = max - min

  if (d === 0) return { h: 0, s: 0, l: Math.round(l * 100) }

  const s = l > 0.5 ? d / (2 - max - min) : d / (max + min)
  let h = 0

  if (max === r) h = ((g - b) / d + (g < b ? 6 : 0)) / 6
  else if (max === g) h = ((b - r) / d + 2) / 6
  else h = ((r - g) / d + 4) / 6

  return {
    h: Math.round(h * 360),
    s: Math.round(s * 100),
    l: Math.round(l * 100),
  }
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
  if (!img.complete) return { ...DEFAULT_COLOR_INFO }

  try {
    const colorThief = new ColorThief()
    const dominantColorArray = colorThief.getColor(img)

    if (dominantColorArray?.length !== 3) return { ...DEFAULT_COLOR_INFO }

    let selectedColor = dominantColorArray

    try {
      const palette = colorThief.getPalette(img, COLOR_CONFIG.paletteSize)
      if (palette?.length > 0) {
        let maxSaturation = 0
        for (const color of palette) {
          if (color?.length === 3) {
            const hsl = rgbToHsl(color[0], color[1], color[2])
            if (hsl.s > maxSaturation) {
              maxSaturation = hsl.s
              selectedColor = color
            }
          }
        }
      }
    } catch {
      // 使用主要颜色
    }

    const [r, g, b] = selectedColor
    return {
      dominantColor: { r, g, b },
      hsl: rgbToHsl(r, g, b),
    }
  } catch {
    return { ...DEFAULT_COLOR_INFO }
  }
}

const detectImageColorInfo = async (imageUrl) => {
  try {
    const img = await loadImage(imageUrl)
    return analyzeImageColors(img)
  } catch {
    return { ...DEFAULT_COLOR_INFO }
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

  const setBackground = async (imageUrl) => {
    document.body.style.background = `url(${imageUrl}) center/cover fixed no-repeat`
    if (isLocalImage(imageUrl)) {
      try {
        const colorInfo = await detectImageColorInfo(imageUrl)
        setTextColorTheme(colorInfo)
      } catch {
        // 静默失败
      }
    }
  }

  const recheckBackgroundBrightness = async () => {
    const currentBg = getCurrentBackground()
    if (!isLocalImage(currentBg)) return
    try {
      const colorInfo = await detectImageColorInfo(currentBg)
      setTextColorTheme(colorInfo)
    } catch {
      // 静默失败
    }
  }

  const loadBackground = () => setBackground(getCurrentBackground())

  const saveBackground = async (imageData) => {
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
