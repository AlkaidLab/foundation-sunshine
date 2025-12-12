import { ref, computed } from 'vue'
import { AppService } from '../services/appService.js'
import { APP_CONSTANTS, ENV_VARS_CONFIG } from '../utils/constants.js'
import { debounce, deepClone } from '../utils/helpers.js'
import { trackEvents } from '../config/firebase.js'
import { searchCoverImage, batchSearchCoverImages } from '../utils/coverSearch.js'

const MESSAGE_DURATION = 3000

/**
 * 应用管理组合式函数
 */
export function useApps() {
  // 状态
  const apps = ref([])
  const originalApps = ref([])
  const filteredApps = ref([])
  const searchQuery = ref('')
  const editingApp = ref(null)
  const platform = ref('')
  const isSaving = ref(false)
  const isDragging = ref(false)
  const viewMode = ref('grid')
  const message = ref('')
  const messageType = ref('success')
  const envVars = ref({})
  const debouncedSearch = ref(null)
  const isScanning = ref(false)
  const scannedApps = ref([])
  const showScanResult = ref(false)
  const scannedAppsSearchQuery = ref('')
  const showGamesOnly = ref(false)
  const selectedAppType = ref('all') // 'all', 'executable', 'shortcut', 'batch', 'command', 'url'

  // 计算属性
  const messageClass = computed(() => ({
    [`alert-${messageType.value}`]: true,
  }))

  // 消息图标映射
  const MESSAGE_ICONS = {
    success: 'fa-check-circle',
    error: 'fa-exclamation-circle',
    warning: 'fa-exclamation-triangle',
    info: 'fa-info-circle',
  }

  const showMessage = (msg, type = APP_CONSTANTS.MESSAGE_TYPES.SUCCESS) => {
    message.value = msg
    messageType.value = type
    setTimeout(() => {
      message.value = ''
    }, MESSAGE_DURATION)
  }

  const getMessageIcon = () => MESSAGE_ICONS[messageType.value] || MESSAGE_ICONS.success

  const createDefaultApp = (overrides = {}) => ({
    ...APP_CONSTANTS.DEFAULT_APP,
    index: -1,
    ...overrides,
  })

  // 初始化
  const init = (t) => {
    envVars.value = Object.fromEntries(
      Object.entries(ENV_VARS_CONFIG).map(([key, translationKey]) => [key, t(translationKey)])
    )
    debouncedSearch.value = debounce(performSearch, APP_CONSTANTS.SEARCH_DEBOUNCE_TIME)
  }

  // 数据加载
  const loadApps = async () => {
    try {
      apps.value = await AppService.getApps()
      originalApps.value = deepClone(apps.value)
      filteredApps.value = [...apps.value]
    } catch (error) {
      console.error('加载应用失败:', error)
      showMessage('加载应用失败', APP_CONSTANTS.MESSAGE_TYPES.ERROR)
    }
  }

  const loadPlatform = async () => {
    try {
      platform.value = await AppService.getPlatform()
    } catch (error) {
      console.error('加载平台信息失败:', error)
      platform.value = APP_CONSTANTS.PLATFORMS.WINDOWS
    }
  }

  // 搜索
  const performSearch = () => {
    filteredApps.value = AppService.searchApps(apps.value, searchQuery.value)
  }

  const clearSearch = () => {
    searchQuery.value = ''
    performSearch()
  }

  // 应用操作
  const getOriginalIndex = (app) => apps.value.indexOf(app)

  const newApp = () => {
    trackEvents.userAction('new_app_clicked')
    editingApp.value = createDefaultApp()
  }

  const editApp = (index) => {
    editingApp.value = { ...deepClone(apps.value[index]), index }
  }

  const closeAppEditor = () => {
    editingApp.value = null
  }

  const handleSaveApp = async (appData) => {
    try {
      isSaving.value = true
      await AppService.saveApps(apps.value, appData)
      await loadApps()
      editingApp.value = null
      showMessage('应用保存成功', APP_CONSTANTS.MESSAGE_TYPES.SUCCESS)
    } catch (error) {
      console.error('保存应用失败:', error)
      showMessage('保存应用失败', APP_CONSTANTS.MESSAGE_TYPES.ERROR)
    } finally {
      isSaving.value = false
    }
  }

  const showDeleteForm = async (index) => {
    if (await confirm(`确定要删除应用 "${apps.value[index].name}" 吗？`)) {
      await deleteApp(index)
    }
  }

  const deleteApp = async (index) => {
    const appName = apps.value[index]?.name || 'unknown'
    try {
      apps.value.splice(index, 1)
      await AppService.saveApps(apps.value, null)
      await loadApps()
      showMessage('应用删除成功', APP_CONSTANTS.MESSAGE_TYPES.SUCCESS)
      trackEvents.appDeleted(appName)
    } catch (error) {
      console.error('删除应用失败:', error)
      showMessage('删除应用失败', APP_CONSTANTS.MESSAGE_TYPES.ERROR)
    }
  }

  // 检测是否有未保存的更改
  const hasUnsavedChanges = () => {
    if (apps.value.length !== originalApps.value.length) {
      return true
    }
  
    // 深度比较应用列表
    const appsStr = JSON.stringify(apps.value.map(app => ({ ...app, index: undefined })))
    const originalStr = JSON.stringify(originalApps.value.map(app => ({ ...app, index: undefined })))
    
    return appsStr !== originalStr
  }

  const save = async () => {
    // 如果没有更改，直接返回
    if (!hasUnsavedChanges()) {
      showMessage('没有需要保存的更改', APP_CONSTANTS.MESSAGE_TYPES.INFO)
      return
    }

    try {
      isSaving.value = true
      await AppService.saveApps(apps.value, null)
      // 保存成功后更新原始列表
      originalApps.value = deepClone(apps.value)
      showMessage('应用列表保存成功', APP_CONSTANTS.MESSAGE_TYPES.SUCCESS)
      trackEvents.userAction('apps_saved', { count: apps.value.length })
    } catch (error) {
      console.error('保存应用列表失败:', error)
      showMessage('保存应用列表失败', APP_CONSTANTS.MESSAGE_TYPES.ERROR)
    } finally {
      isSaving.value = false
    }
  }

  // 拖拽排序
  const onDragStart = () => {
    isDragging.value = true
  }

  const onDragEnd = async () => {
    isDragging.value = false
    await save()
  }

  // 封面搜索相关（使用共享的 coverSearch 模块）

  // Tauri 环境检测
  const isTauriEnv = () => !!window.__TAURI__?.core?.invoke

  // 扫描目录功能
  const scanDirectory = async (extractIcons = true) => {
    const tauri = window.__TAURI__
    if (!tauri?.core?.invoke) {
      showMessage('扫描功能仅在 Tauri 环境下可用', APP_CONSTANTS.MESSAGE_TYPES.WARNING)
      return
    }

    if (!tauri?.dialog?.open) {
      showMessage('无法打开文件对话框', APP_CONSTANTS.MESSAGE_TYPES.ERROR)
      return
    }

    try {
      const selectedDir = await tauri.dialog.open({
        directory: true,
        multiple: false,
        title: '选择要扫描的目录',
      })

      if (!selectedDir) return

      isScanning.value = true
      showMessage('正在扫描目录...', APP_CONSTANTS.MESSAGE_TYPES.INFO)

      const foundApps = await tauri.core.invoke('scan_directory_for_apps', {
        directory: selectedDir,
        extractIcons,
      })

      if (foundApps.length === 0) {
        scannedApps.value = []
        showScanResult.value = true
        showMessage('未找到可添加的应用程序', APP_CONSTANTS.MESSAGE_TYPES.INFO)
      } else {
        // 先显示扫描结果（无封面）
        scannedApps.value = foundApps
        showScanResult.value = true
        showMessage(`找到 ${foundApps.length} 个应用程序，正在搜索封面...`, APP_CONSTANTS.MESSAGE_TYPES.INFO)

        // 异步更新封面图片
        asyncUpdateCovers(foundApps)
      }

      trackEvents.userAction('directory_scanned', { count: foundApps.length, extractIcons })
    } catch (error) {
      console.error('扫描目录失败:', error)
      showMessage(`扫描失败: ${error}`, APP_CONSTANTS.MESSAGE_TYPES.ERROR)
    } finally {
      isScanning.value = false
    }
  }

  // 异步更新封面图片
  const asyncUpdateCovers = async (appList) => {
    let coversFound = 0
    const total = appList.length

    // 并行搜索所有封面，但逐个更新UI
    const promises = appList.map(async (app, index) => {
      try {
        const imagePath = await searchCoverImage(encodeURIComponent(app.name))
        if (imagePath && scannedApps.value[index]) {
          // 更新对应位置的应用封面
          scannedApps.value[index] = { ...scannedApps.value[index], 'image-path': imagePath }
          coversFound++
        }
      } catch (error) {
        console.warn(`搜索封面失败: ${app.name}`, error)
      }
    })

    await Promise.allSettled(promises)

    // 搜索完成后显示结果
    showMessage(
      `已匹配 ${coversFound}/${total} 个封面`,
      coversFound > 0 ? APP_CONSTANTS.MESSAGE_TYPES.SUCCESS : APP_CONSTANTS.MESSAGE_TYPES.INFO
    )
  }

  // 扫描应用字段处理
  const getScannedAppField = (app, field) => app[field] || app[field.replace(/-/g, '_')] || ''

  const getScannedAppImage = (app) => getScannedAppField(app, 'image-path')

  const createAppFromScanned = (scannedApp) => ({
    ...APP_CONSTANTS.DEFAULT_APP,
    name: scannedApp.name,
    cmd: scannedApp.cmd,
    'working-dir': getScannedAppField(scannedApp, 'working-dir'),
    'image-path': getScannedAppField(scannedApp, 'image-path'),
  })

  const removeFromScannedList = (sourcePath) => {
    const index = scannedApps.value.findIndex((a) => a.source_path === sourcePath)
    if (index !== -1) {
      scannedApps.value.splice(index, 1)
      if (scannedApps.value.length === 0) {
        showScanResult.value = false
      }
    }
  }

  const addScannedApp = (scannedApp) => {
    editingApp.value = createDefaultApp({
      name: scannedApp.name,
      cmd: scannedApp.cmd,
      'working-dir': getScannedAppField(scannedApp, 'working-dir'),
      'image-path': getScannedAppField(scannedApp, 'image-path'),
    })

    removeFromScannedList(scannedApp.source_path)
    showMessage(`正在编辑应用: ${scannedApp.name}`, APP_CONSTANTS.MESSAGE_TYPES.INFO)
    trackEvents.userAction('scanned_app_edit', { name: scannedApp.name })
  }

  const quickAddScannedApp = async (scannedApp, index) => {
    try {
      apps.value.push(createAppFromScanned(scannedApp))
      await AppService.saveApps(apps.value, null)
      await loadApps()

      scannedApps.value.splice(index, 1)
      if (scannedApps.value.length === 0) {
        showScanResult.value = false
      }

      showMessage(`已添加应用: ${scannedApp.name}`, APP_CONSTANTS.MESSAGE_TYPES.SUCCESS)
      trackEvents.userAction('scanned_app_quick_added', { name: scannedApp.name })
    } catch (error) {
      console.error('快速添加应用失败:', error)
      showMessage('添加失败', APP_CONSTANTS.MESSAGE_TYPES.ERROR)
    }
  }

  const addAllScannedApps = async () => {
    if (scannedApps.value.length === 0) return

    try {
      isSaving.value = true
      const appsToAdd = scannedApps.value.map(createAppFromScanned)

      apps.value.push(...appsToAdd)
      await AppService.saveApps(apps.value, null)
      await loadApps()

      showMessage(`已添加 ${appsToAdd.length} 个应用`, APP_CONSTANTS.MESSAGE_TYPES.SUCCESS)
      trackEvents.userAction('scanned_apps_batch_added', { count: appsToAdd.length })

      scannedApps.value = []
      showScanResult.value = false
    } catch (error) {
      console.error('批量添加应用失败:', error)
      showMessage('批量添加失败', APP_CONSTANTS.MESSAGE_TYPES.ERROR)
    } finally {
      isSaving.value = false
    }
  }

  const closeScanResult = () => {
    showScanResult.value = false
    scannedApps.value = []
    scannedAppsSearchQuery.value = ''
    showGamesOnly.value = false
    selectedAppType.value = 'all'
  }

  // 获取各分类的统计信息
  const scanResultStats = computed(() => ({
    all: scannedApps.value.length,
    games: scannedApps.value.filter((app) => app['is-game'] === true).length,
    executable: scannedApps.value.filter((app) => app['app-type'] === 'executable').length,
    shortcut: scannedApps.value.filter((app) => app['app-type'] === 'shortcut').length,
    batch: scannedApps.value.filter((app) => app['app-type'] === 'batch').length,
    command: scannedApps.value.filter((app) => app['app-type'] === 'command').length,
    url: scannedApps.value.filter((app) => app['app-type'] === 'url').length,
  }))

  // 过滤扫描结果
  const filteredScannedApps = computed(() => {
    let filtered = scannedApps.value
    
    // 先按应用类型过滤
    if (selectedAppType.value !== 'all') {
      filtered = filtered.filter((app) => app['app-type'] === selectedAppType.value)
    }
    
    // 再按游戏过滤
    if (showGamesOnly.value) {
      filtered = filtered.filter((app) => app['is-game'] === true)
    }
    
    // 最后按搜索关键词过滤
    if (scannedAppsSearchQuery.value) {
      const query = scannedAppsSearchQuery.value.toLowerCase()
      filtered = filtered.filter((app) => {
        const name = (app.name || '').toLowerCase()
        const cmd = (app.cmd || '').toLowerCase()
        const sourcePath = (app.source_path || '').toLowerCase()
        return name.includes(query) || cmd.includes(query) || sourcePath.includes(query)
      })
    }
    
    return filtered
  })

  const removeScannedApp = (index) => {
    scannedApps.value.splice(index, 1)
    if (scannedApps.value.length === 0) {
      showScanResult.value = false
    }
  }

  const searchCoverForScannedApp = async (index) => {
    const app = scannedApps.value[index]
    if (!app) return

    try {
      showMessage(`正在搜索封面: ${app.name}`, APP_CONSTANTS.MESSAGE_TYPES.INFO)
      const imagePath = await searchCoverImage(app.name)

      if (imagePath) {
        scannedApps.value[index] = { ...app, 'image-path': imagePath }
        showMessage(`已找到封面: ${app.name}`, APP_CONSTANTS.MESSAGE_TYPES.SUCCESS)
      } else {
        showMessage(`未找到封面: ${app.name}`, APP_CONSTANTS.MESSAGE_TYPES.WARNING)
      }
    } catch (error) {
      console.error('搜索封面失败:', error)
      showMessage('搜索封面失败', APP_CONSTANTS.MESSAGE_TYPES.ERROR)
    }
  }

  const handleCopySuccess = () => showMessage('复制成功', APP_CONSTANTS.MESSAGE_TYPES.SUCCESS)
  const handleCopyError = () => showMessage('复制失败', APP_CONSTANTS.MESSAGE_TYPES.ERROR)

  return {
    // 状态
    apps,
    filteredApps,
    searchQuery,
    editingApp,
    platform,
    isSaving,
    isDragging,
    viewMode,
    message,
    messageType,
    envVars,
    debouncedSearch,
    isScanning,
    scannedApps,
    showScanResult,
    scannedAppsSearchQuery,
    showGamesOnly,
    selectedAppType,
    // 计算属性
    messageClass,
    filteredScannedApps,
    scanResultStats,
    // 方法
    init,
    loadApps,
    loadPlatform,
    performSearch,
    clearSearch,
    getOriginalIndex,
    newApp,
    editApp,
    closeAppEditor,
    handleSaveApp,
    showDeleteForm,
    deleteApp,
    save,
    hasUnsavedChanges,
    onDragStart,
    onDragEnd,
    scanDirectory,
    addScannedApp,
    quickAddScannedApp,
    addAllScannedApps,
    closeScanResult,
    removeScannedApp,
    getScannedAppImage,
    searchCoverForScannedApp,
    isTauriEnv,
    showMessage,
    getMessageIcon,
    handleCopySuccess,
    handleCopyError,
  }
}
