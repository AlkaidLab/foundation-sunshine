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

const ALLOWED_EXTERNAL_PROTOCOLS = new Set(['http:', 'https:', 'ms-windows-store:']);

/**
 * 检查 URL 是否允许交给系统打开。
 * @param {string} url 要检查的 URL
 * @returns {boolean} 是否为允许的外部 URL
 */
export function isAllowedExternalUrl(url) {
  try {
    return ALLOWED_EXTERNAL_PROTOCOLS.has(new URL(String(url).trim()).protocol);
  } catch {
    return false;
  }
}

/**
 * 检测是否在 Tauri 环境中
 * @returns {boolean} 是否在 Tauri 环境
 */
export function isTauriEnv(windowObject = globalThis.window) {
  return !!(windowObject?.isTauri || windowObject?.__TAURI__);
}

/**
 * 打开外部链接（支持 Tauri 和浏览器环境）
 * @param {string} url 要打开的 URL
 * @param {Window} [windowObject] 当前窗口
 * @returns {Promise<boolean>} 是否已交给系统或浏览器打开
 */
export async function openExternalUrl(url, windowObject = globalThis.window) {
  const target = String(url).trim();
  if (!isAllowedExternalUrl(target)) {
    throw new Error('Unsupported external URL');
  }

  const invoke = windowObject?.__TAURI__?.core?.invoke;
  if (typeof invoke === 'function') {
    const opened = await invoke('open_external_url', { url: target });
    if (opened === false) {
      throw new Error('External URL was not opened');
    }
    return true;
  }

  const openedWindow = windowObject?.open?.(target, '_blank', 'noopener,noreferrer');
  if (openedWindow) openedWindow.opener = null;
  return Boolean(openedWindow);
}

const installedExternalLinkHandlers = new WeakMap();

/**
 * 在 Tauri WebUI 中把外部链接统一交给受限的原生命令处理。
 * @param {Document} [documentObject] 当前文档
 * @param {Window} [windowObject] 当前窗口
 * @returns {Function|undefined} 取消监听函数
 */
export function installExternalLinkHandler(
  documentObject = globalThis.document,
  windowObject = globalThis.window,
) {
  if (!documentObject?.addEventListener || !isTauriEnv(windowObject)) return undefined;
  if (installedExternalLinkHandlers.has(documentObject)) {
    return installedExternalLinkHandlers.get(documentObject).dispose;
  }

  const handler = (event) => {
    if (event.defaultPrevented || (event.button !== undefined && event.button !== 0)) return;

    const anchor = event.target?.closest?.('a[href]');
    if (!anchor || anchor.hasAttribute('download')) return;

    const url = anchor.href;
    if (!isAllowedExternalUrl(url)) return;

    const parsedUrl = new URL(url);
    if (parsedUrl.origin !== 'null' && parsedUrl.origin === windowObject.location?.origin) return;

    event.preventDefault();
    void openExternalUrl(url, windowObject).catch((error) => {
      console.error('Failed to open external URL:', error);
    });
  };

  const dispose = () => {
    documentObject.removeEventListener('click', handler);
    installedExternalLinkHandlers.delete(documentObject);
  };

  documentObject.addEventListener('click', handler);
  installedExternalLinkHandlers.set(documentObject, { dispose });
  return dispose;
}
