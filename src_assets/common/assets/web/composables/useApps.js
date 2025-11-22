import { ref, computed } from 'vue'
import { AppService } from '../services/appService.js'
import { APP_CONSTANTS, ENV_VARS_CONFIG } from '../utils/constants.js'
import { debounce, deepClone } from '../utils/helpers.js'
import { trackEvents } from '../config/firebase.js'

/**
 * 应用管理组合式函数
 */
export function useApps() {
  const apps = ref([])
  const filteredApps = ref([])
  const searchQuery = ref('')
  const editingApp = ref(null)
  const platform = ref('')
  const isSaving = ref(false)
  const isDragging = ref(false)
  const viewMode = ref('grid') // 'grid' or 'list'
  const message = ref('')
  const messageType = ref('success')
  const envVars = ref({})
  const debouncedSearch = ref(null)

  // 消息样式类
  const messageClass = computed(() => {
    return {
      'alert-success': messageType.value === 'success',
      'alert-error': messageType.value === 'error',
      'alert-warning': messageType.value === 'warning',
      'alert-info': messageType.value === 'info'
    }
  })

  // 初始化环境变量
  const initEnvVars = (t) => {
    envVars.value = {}
    for (const [key, translationKey] of Object.entries(ENV_VARS_CONFIG)) {
      envVars.value[key] = t(translationKey)
    }
  }

  // 加载应用列表
  const loadApps = async () => {
    try {
      apps.value = await AppService.getApps()
      filteredApps.value = [...apps.value]
    } catch (error) {
      console.error('加载应用失败:', error)
      showMessage('加载应用失败', APP_CONSTANTS.MESSAGE_TYPES.ERROR)
    }
  }

  // 加载平台信息
  const loadPlatform = async () => {
    try {
      platform.value = await AppService.getPlatform()
    } catch (error) {
      console.error('加载平台信息失败:', error)
      platform.value = APP_CONSTANTS.PLATFORMS.WINDOWS
    }
  }

  // 执行搜索
  const performSearch = () => {
    filteredApps.value = AppService.searchApps(apps.value, searchQuery.value)
  }

  // 清除搜索
  const clearSearch = () => {
    searchQuery.value = ''
    performSearch()
  }

  // 获取原始索引
  const getOriginalIndex = (app, filteredIndex) => {
    return apps.value.findIndex(a => a === app)
  }

  // 新增应用
  const newApp = () => {
    trackEvents.userAction('new_app_clicked')
    editingApp.value = {
      ...APP_CONSTANTS.DEFAULT_APP,
      index: -1
    }
  }

  // 编辑应用
  const editApp = (index) => {
    editingApp.value = deepClone(apps.value[index])
    editingApp.value.index = index
  }

  // 关闭应用编辑器
  const closeAppEditor = () => {
    editingApp.value = null
  }

  // 处理保存应用
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

  // 显示删除确认
  const showDeleteForm = async (index) => {
    const confirmed = await confirm(
      `确定要删除应用 "${apps.value[index].name}" 吗？`
    )
    if (confirmed) {
      await deleteApp(index)
    }
  }

  // 删除应用
  const deleteApp = async (index) => {
    try {
      apps.value.splice(index, 1)
      await AppService.saveApps(apps.value, null)
      await loadApps()
      showMessage('应用删除成功', APP_CONSTANTS.MESSAGE_TYPES.SUCCESS)
      trackEvents.appDeleted(apps.value[index]?.name || 'unknown')
    } catch (error) {
      console.error('删除应用失败:', error)
      showMessage('删除应用失败', APP_CONSTANTS.MESSAGE_TYPES.ERROR)
    }
  }

  // 保存应用列表
  const save = async () => {
    try {
      isSaving.value = true
      await AppService.saveApps(apps.value, null)
      showMessage('应用列表保存成功', APP_CONSTANTS.MESSAGE_TYPES.SUCCESS)
      trackEvents.userAction('apps_saved', { count: apps.value.length })
    } catch (error) {
      console.error('保存应用列表失败:', error)
      showMessage('保存应用列表失败', APP_CONSTANTS.MESSAGE_TYPES.ERROR)
    } finally {
      isSaving.value = false
    }
  }

  // 拖拽开始
  const onDragStart = () => {
    isDragging.value = true
  }

  // 拖拽结束
  const onDragEnd = async () => {
    isDragging.value = false
    // 拖拽后自动保存
    await save()
  }

  // 显示消息
  const showMessage = (msg, type = APP_CONSTANTS.MESSAGE_TYPES.SUCCESS) => {
    message.value = msg
    messageType.value = type
    setTimeout(() => {
      message.value = ''
    }, 3000)
  }

  // 获取消息图标
  const getMessageIcon = () => {
    const icons = {
      success: 'fa-check-circle',
      error: 'fa-exclamation-circle',
      warning: 'fa-exclamation-triangle',
      info: 'fa-info-circle'
    }
    return icons[messageType.value] || icons.success
  }

  // 处理复制成功
  const handleCopySuccess = () => {
    showMessage('复制成功', APP_CONSTANTS.MESSAGE_TYPES.SUCCESS)
  }

  // 处理复制错误
  const handleCopyError = () => {
    showMessage('复制失败', APP_CONSTANTS.MESSAGE_TYPES.ERROR)
  }

  // 初始化
  const init = (t) => {
    initEnvVars(t)
    debouncedSearch.value = debounce(performSearch, APP_CONSTANTS.SEARCH_DEBOUNCE_TIME)
  }

  return {
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
    messageClass,
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
    onDragStart,
    onDragEnd,
    showMessage,
    getMessageIcon,
    handleCopySuccess,
    handleCopyError,
    init,
  }
}

