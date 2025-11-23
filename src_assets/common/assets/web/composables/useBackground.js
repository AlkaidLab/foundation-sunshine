const DEFAULT_BACKGROUND = 'https://raw.gitmirror.com/qiin2333/qiin.github.io/assets/img/sunshine-bg0.webp'
const STORAGE_KEY = 'customBackground'

/**
 * 背景图片管理组合式函数
 * @param {Object} options - 配置选项
 * @param {string} options.defaultBackground - 默认背景URL
 * @param {string} options.storageKey - localStorage 存储键名
 * @param {number} options.maxWidth - 图片最大宽度
 * @param {number} options.maxHeight - 图片最大高度
 * @param {number} options.maxSizeMB - 最大文件大小（MB）
 * @returns {Object} 背景管理相关的函数和状态
 */
export function useBackground(options = {}) {
  const {
    defaultBackground = DEFAULT_BACKGROUND,
    storageKey = STORAGE_KEY,
    maxWidth = 1920,
    maxHeight = 1080,
    maxSizeMB = 2
  } = options

  /**
   * 设置背景图片
   * @param {string} imageUrl - 图片URL或base64
   */
  const setBackground = (imageUrl) => {
    document.body.style.background = `url(${imageUrl}) center/cover fixed no-repeat`
  }

  /**
   * 加载背景图片
   */
  const loadBackground = () => {
    const savedBg = localStorage.getItem(storageKey) ?? defaultBackground
    setBackground(savedBg)
  }

  /**
   * 保存背景图片到 localStorage
   * @param {string} imageData - base64 图片数据
   */
  const saveBackground = (imageData) => {
    try {
      localStorage.setItem(storageKey, imageData)
      setBackground(imageData)
    } catch (storageError) {
      if (storageError.name === 'QuotaExceededError') {
        // 如果超出配额，清除旧的背景并重试
        localStorage.removeItem(storageKey)
        try {
          localStorage.setItem(storageKey, imageData)
          setBackground(imageData)
        } catch (retryError) {
          console.error('无法存储背景图片:', retryError)
          throw new Error('图片太大，无法存储。请选择更小的图片或降低图片质量。')
        }
      } else {
        throw storageError
      }
    }
  }

  /**
   * 压缩图片
   * @param {File} file - 图片文件
   * @param {number} initialQuality - 初始压缩质量 (0-1)
   * @returns {Promise<string>} base64 字符串
   */
  const compressImage = (file, initialQuality = 0.8) => {
    return new Promise((resolve, reject) => {
      const reader = new FileReader()
      reader.onload = (event) => {
        const img = new Image()
        img.onload = () => {
          // 计算新尺寸
          let width = img.width
          let height = img.height
          
          if (width > maxWidth || height > maxHeight) {
            const ratio = Math.min(maxWidth / width, maxHeight / height)
            width = width * ratio
            height = height * ratio
          }

          // 尝试不同质量级别，直到找到合适的大小
          const tryCompress = (quality) => {
            const canvas = document.createElement('canvas')
            canvas.width = width
            canvas.height = height
            const ctx = canvas.getContext('2d')
            ctx.drawImage(img, 0, 0, width, height)

            const compressedDataUrl = canvas.toDataURL('image/jpeg', quality)
            
            // 检查大小
            const sizeInMB = (compressedDataUrl.length * 3) / 4 / 1024 / 1024
            
            if (sizeInMB > maxSizeMB && quality > 0.3) {
              // 如果还是太大且质量还可以降低，继续尝试
              return tryCompress(quality - 0.1)
            } else if (sizeInMB > maxSizeMB) {
              // 如果质量已经很低但还是太大，拒绝
              reject(new Error('图片太大，无法存储。请选择更小的图片。'))
              return null
            } else {
              return compressedDataUrl
            }
          }

          const result = tryCompress(initialQuality)
          if (result) {
            resolve(result)
          }
        }
        img.onerror = () => reject(new Error('图片加载失败'))
        img.src = event.target.result
      }
      reader.onerror = () => reject(new Error('文件读取失败'))
      reader.readAsDataURL(file)
    })
  }

  /**
   * 处理拖拽悬停
   * @param {DragEvent} e - 拖拽事件
   */
  const handleDragOver = (e) => {
    e.preventDefault()
    if (!e.dataTransfer?.types?.includes('Files')) {
      return
    }
    document.body.classList.add('dragover')
  }

  /**
   * 处理拖拽离开
   */
  const handleDragLeave = () => {
    document.body.classList.remove('dragover')
  }

  /**
   * 处理文件拖放
   * @param {DragEvent} e - 拖拽事件
   * @param {Function} onError - 错误回调函数
   */
  const handleDrop = async (e, onError) => {
    e.preventDefault()
    if (!e.dataTransfer?.types?.includes('Files')) {
      return
    }
    document.body.classList.remove('dragover')

    const file = e.dataTransfer?.files?.[0]
    if (!file?.type.startsWith('image/')) return

    try {
      // 压缩图片
      const compressedImageData = await compressImage(file)
      
      // 保存背景
      saveBackground(compressedImageData)
    } catch (error) {
      console.error('处理图片失败:', error)
      if (onError) {
        onError(error)
      } else {
        alert(error.message || '处理图片时发生错误')
      }
    }
  }

  /**
   * 添加拖拽监听器
   */
  const addDragListeners = (onError) => {
    const dragOverHandler = (e) => handleDragOver(e)
    const dragLeaveHandler = () => handleDragLeave()
    const dropHandler = (e) => handleDrop(e, onError)

    document.addEventListener('dragover', dragOverHandler)
    document.addEventListener('dragleave', dragLeaveHandler)
    document.addEventListener('drop', dropHandler)

    // 返回清理函数
    return () => {
      document.removeEventListener('dragover', dragOverHandler)
      document.removeEventListener('dragleave', dragLeaveHandler)
      document.removeEventListener('drop', dropHandler)
    }
  }

  /**
   * 清除背景图片
   */
  const clearBackground = () => {
    localStorage.removeItem(storageKey)
    setBackground(defaultBackground)
  }

  /**
   * 获取当前背景
   * @returns {string} 当前背景URL或base64
   */
  const getCurrentBackground = () => {
    return localStorage.getItem(storageKey) ?? defaultBackground
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
    getCurrentBackground
  }
}

