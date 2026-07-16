/**
 * 防抖函数
 * @param {Function} func 需要防抖的函数
 * @param {number} wait 等待时间（毫秒）
 * @returns {Function} 防抖后的函数
 */
export function debounce(func, wait) {
  let timeout;
  return function executedFunction(...args) {
    const later = () => {
      clearTimeout(timeout);
      func(...args);
    };
    clearTimeout(timeout);
    timeout = setTimeout(later, wait);
  };
}

/**
 * 深拷贝函数
 * @param {*} obj 需要深拷贝的对象
 * @returns {*} 深拷贝后的对象
 */
export function deepClone(obj) {
  if (obj === null || typeof obj !== 'object') return obj;
  if (obj instanceof Date) return new Date(obj);
  if (obj instanceof Array) return obj.map(item => deepClone(item));
  if (typeof obj === 'object') {
    const clonedObj = {};
    for (const key in obj) {
      if (obj.hasOwnProperty(key)) {
        clonedObj[key] = deepClone(obj[key]);
      }
    }
    return clonedObj;
  }
}

/**
 * 安全的JSON解析
 * @param {string} str JSON字符串
 * @param {*} defaultValue 解析失败时的默认值
 * @returns {*} 解析结果或默认值
 */
export function safeJsonParse(str, defaultValue = null) {
  try {
    return JSON.parse(str);
  } catch (error) {
    console.warn('JSON解析失败:', error);
    return defaultValue;
  }
}

/**
 * 格式化错误信息
 * @param {Error|string} error 错误对象或错误信息
 * @returns {string} 格式化后的错误信息
 */
export function formatError(error) {
  if (typeof error === 'string') return error;
  if (error && error.message) return error.message;
  return '未知错误';
}

/**
 * 检查是否为有效的URL
 * @param {string} url URL字符串
 * @returns {boolean} 是否为有效URL
 */
export function isValidUrl(url) {
  try {
    new URL(url);
    return true;
  } catch {
    return false;
  }
}

/**
 * 检测是否在 Tauri 环境中
 * @returns {boolean} 是否在 Tauri 环境
 */
export function isTauriEnv() {
  return typeof window !== 'undefined' && !!(window.isTauri || window.__TAURI__);
}

/**
 * 打开外部链接（支持 Tauri 和浏览器环境）
 * @param {string} url 要打开的 URL
 * @returns {Promise<void>}
 */
export async function openExternalUrl(url) {
  if (!isValidUrl(url)) {
    throw new Error('Invalid URL');
  }

  if (isTauriEnv()) {
    try {
      await window.__TAURI__.shell.open(url);
    } catch (error) {
      console.error('Failed to open URL with Tauri shell:', error);
      // 降级到 window.open
      window.open(url, '_blank');
    }
  } else {
    // 非 Tauri 环境，使用 window.open
    window.open(url, '_blank');
  }
}
