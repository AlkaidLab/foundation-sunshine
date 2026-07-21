/**
 * 图片工具函数
 * 用于处理应用图片URL的标准化逻辑
 */
/**
 * 获取图片预览URL
 * @param {string} imagePath 图片路径
 * @returns {string} 预览URL
 */
export function getImagePreviewUrl(imagePath = 'box.png') {
  if (imagePath === 'desktop') {
    return '/boxart/desktop.png'
  }
  if (/^(https?:|blob:|data:)/i.test(imagePath)) {
    return imagePath
  }
  if (imagePath.startsWith('/boxart/') || imagePath.startsWith('boxart/')) {
    return imagePath.startsWith('/') ? imagePath : `/${imagePath}`
  }
  // 如果路径不包含分隔符,说明是boxart资源ID
  if (!/[/\\]/.test(imagePath)) {
    return `/boxart/${encodeBoxArtName(imagePath)}`
  }

  if (/[\\/]covers[\\/]/i.test(imagePath)) {
    return `/boxart/${encodeBoxArtName(imagePath.split(/[/\\]/).pop())}`
  }

  return isLocalImagePath(imagePath) ? `file://${imagePath}` : imagePath
}

function encodeBoxArtName(name) {
  const value = String(name || '')
  try {
    return encodeURIComponent(decodeURIComponent(value))
  } catch {
    return encodeURIComponent(value)
  }
}

/**
 * 检查图片路径是否为本地文件路径
 * @param {string} imagePath 图片路径
 * @returns {boolean} 是否为本地文件路径
 */
export function isLocalImagePath(imagePath) {
  if (!imagePath) {
    return false
  }

  // 如果是网络URL或blob/data URL，不是本地路径
  if (
    imagePath.startsWith('http://') ||
    imagePath.startsWith('https://') ||
    imagePath.startsWith('blob:') ||
    imagePath.startsWith('data:')
  ) {
    return false
  }

  return true
}
