/**
 * 文件选择工具模块
 * 提供跨平台的文件和目录选择功能
 */

const FILE_FILTERS = [
  { name: '可执行文件', extensions: ['exe', 'app', 'sh', 'bat', 'cmd'] },
  { name: '所有文件', extensions: ['*'] },
]

const PLACEHOLDERS = {
  windows: { cmd: 'C:\\Program Files\\App\\app.exe', 'working-dir': 'C:\\Program Files\\App' },
  default: { cmd: '/usr/bin/app', 'working-dir': '/usr/bin' },
}

/**
 * 文件选择器类
 */
export class FileSelector {
  constructor(options = {}) {
    this.platform = options.platform || 'linux'
    this.onSuccess = options.onSuccess || (() => {})
    this.onError = options.onError || (() => {})
    this.onInfo = options.onInfo || (() => {})
    this.currentField = null
    this.selectionType = null
  }

  /**
   * 通用选择方法
   */
  async select(fieldName, input, callback, isDirectory = false) {
    this.currentField = fieldName
    this.selectionType = isDirectory ? 'directory' : 'file'

    if (this.isTauriEnvironment()) {
      return isDirectory ? this.selectDirectoryTauri(fieldName, callback) : this.selectFileTauri(fieldName, callback)
    }

    if (this.isElectronEnvironment()) {
      return isDirectory
        ? this.selectDirectoryElectron(fieldName, callback)
        : this.selectFileElectron(fieldName, callback)
    }

    return isDirectory ? this.selectDirectoryBrowser(input, callback) : this.selectFileBrowser(input, callback)
  }

  /**
   * 选择文件
   */
  async selectFile(fieldName, fileInput, callback) {
    return this.select(fieldName, fileInput, callback, false)
  }

  /**
   * 选择目录
   */
  async selectDirectory(fieldName, dirInput, callback) {
    return this.select(fieldName, dirInput, callback, true)
  }

  /**
   * 浏览器环境下选择文件/目录
   */
  selectBrowser(input, callback, isDirectory) {
    if (!input) {
      this.onError(isDirectory ? '目录输入元素不存在' : '文件输入元素不存在')
      return
    }

    input.value = ''
    input.click()

    const handleSelected = (event) => {
      const files = event.target.files
      const hasSelection = isDirectory ? files.length > 0 : files[0]

      if (hasSelection && this.currentField) {
        try {
          const path = isDirectory ? this.processDirectoryPath(files[0]) : this.processFilePath(files[0])

          callback?.(this.currentField, path)
          this.onSuccess(`${isDirectory ? '目录' : '文件'}选择成功: ${path}`)

          if (!this.isElectronEnvironment()) {
            this.onInfo('浏览器环境下无法获取完整路径，请检查并手动调整路径')
          }
        } catch (error) {
          console.error(`${isDirectory ? '目录' : '文件'}选择处理失败:`, error)
          this.onError(`${isDirectory ? '目录' : '文件'}选择处理失败，请重试`)
        }
      }

      this.resetState()
      input.removeEventListener('change', handleSelected)
    }

    input.addEventListener('change', handleSelected)
  }

  selectFileBrowser(fileInput, callback) {
    return this.selectBrowser(fileInput, callback, false)
  }

  selectDirectoryBrowser(dirInput, callback) {
    return this.selectBrowser(dirInput, callback, true)
  }

  /**
   * 检查是否在 Tauri 环境
   */
  isTauriEnvironment() {
    const tauri = typeof window !== 'undefined' ? window.__TAURI__ : null
    return !!(tauri?.dialog?.open || tauri?.core?.invoke)
  }

  /**
   * 检查是否在 Electron 环境
   */
  isElectronEnvironment() {
    return typeof window !== 'undefined' && window.process?.type === 'renderer'
  }

  /**
   * Tauri 环境下选择
   */
  async selectTauri(fieldName, callback, isDirectory) {
    const tauri = window.__TAURI__
    if (!tauri?.dialog?.open) {
      this.onError('Tauri 对话框 API 不可用')
      this.resetState()
      return null
    }

    try {
      const options = isDirectory
        ? { title: '选择目录', multiple: false, directory: true }
        : { title: '选择文件', filters: FILE_FILTERS, multiple: false, directory: false }

      const selected = await tauri.dialog.open(options)

      if (selected) {
        callback?.(fieldName, selected)
        this.onSuccess(`${isDirectory ? '目录' : '文件'}选择成功: ${selected}`)
        this.resetState()
        return selected
      }
    } catch (error) {
      console.error(`Tauri ${isDirectory ? '目录' : '文件'}选择失败:`, error)
      this.onError(`${isDirectory ? '目录' : '文件'}选择失败，请手动输入路径`)
    }

    this.resetState()
    return null
  }

  async selectFileTauri(fieldName, callback) {
    return this.selectTauri(fieldName, callback, false)
  }

  async selectDirectoryTauri(fieldName, callback) {
    return this.selectTauri(fieldName, callback, true)
  }

  /**
   * Electron 环境下选择
   */
  async selectElectron(fieldName, callback, isDirectory) {
    try {
      const { dialog } = window.require('electron').remote
      const options = isDirectory
        ? { properties: ['openDirectory'] }
        : { properties: ['openFile'], filters: FILE_FILTERS }

      const result = await dialog.showOpenDialog(options)

      if (!result.canceled && result.filePaths.length > 0) {
        const path = result.filePaths[0]
        callback?.(fieldName, path)
        this.onSuccess(`${isDirectory ? '目录' : '文件'}选择成功: ${path}`)
        return path
      }
    } catch (error) {
      console.error(`${isDirectory ? '目录' : '文件'}选择失败:`, error)
      this.onError(`${isDirectory ? '目录' : '文件'}选择失败，请手动输入路径`)
    }

    this.resetState()
    return null
  }

  async selectFileElectron(fieldName, callback) {
    return this.selectElectron(fieldName, callback, false)
  }

  async selectDirectoryElectron(fieldName, callback) {
    return this.selectElectron(fieldName, callback, true)
  }

  /**
   * 处理文件路径
   */
  processFilePath(file) {
    return file.webkitRelativePath || file.name
  }

  /**
   * 处理目录路径
   */
  processDirectoryPath(firstFile) {
    if (!firstFile.webkitRelativePath) return ''
    const parts = firstFile.webkitRelativePath.split('/')
    return parts.slice(0, -1).join('/')
  }

  /**
   * 检查是否是开发环境
   */
  isDevelopmentEnvironment() {
    if (typeof process !== 'undefined' && process.env?.NODE_ENV === 'development') {
      return true
    }
    if (typeof window !== 'undefined') {
      const { hostname } = window.location
      return hostname === 'localhost' || hostname === '127.0.0.1'
    }
    return false
  }

  /**
   * 重置状态
   */
  resetState() {
    this.currentField = null
    this.selectionType = null
  }

  /**
   * 检查文件选择支持
   */
  checkFileSelectionSupport() {
    if (typeof window === 'undefined') return false
    return !!(window.File && window.FileReader && window.FileList && window.Blob)
  }

  /**
   * 检查目录选择支持
   */
  checkDirectorySelectionSupport(dirInput) {
    return !!(dirInput && 'webkitdirectory' in dirInput)
  }

  /**
   * 获取字段占位符文本
   */
  getPlaceholderText(fieldName) {
    const platformPlaceholders = this.platform === 'windows' ? PLACEHOLDERS.windows : PLACEHOLDERS.default
    return platformPlaceholders[fieldName] || ''
  }

  /**
   * 获取按钮标题文本
   */
  getButtonTitle(type) {
    return type === 'file' ? '选择文件' : type === 'directory' ? '选择目录' : '选择'
  }

  /**
   * 清理文件输入
   */
  cleanupFileInputs(fileInput, dirInput) {
    if (fileInput) fileInput.value = ''
    if (dirInput) dirInput.value = ''
  }
}

/**
 * 创建文件选择器实例的工厂函数
 */
export function createFileSelector(options = {}) {
  return new FileSelector(options)
}

/**
 * 简化的文件选择函数
 */
export async function selectFile(options = {}) {
  const selector = createFileSelector(options)
  return selector.selectFile(options.fieldName, options.fileInput, options.callback)
}

/**
 * 简化的目录选择函数
 */
export async function selectDirectory(options = {}) {
  const selector = createFileSelector(options)
  return selector.selectDirectory(options.fieldName, options.dirInput, options.callback)
}

/**
 * 检查环境支持
 */
export function checkEnvironmentSupport() {
  const selector = createFileSelector()
  return {
    fileSelection: selector.checkFileSelectionSupport(),
    directorySelection: selector.checkDirectorySelectionSupport(),
    isTauri: selector.isTauriEnvironment(),
    isElectron: selector.isElectronEnvironment(),
    isDevelopment: selector.isDevelopmentEnvironment(),
  }
}

export default FileSelector
