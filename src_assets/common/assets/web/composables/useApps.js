import { computed, reactive, ref } from 'vue'
import { AppService } from '../services/appService.js'
import { APP_CONSTANTS, ENV_VARS_CONFIG } from '../utils/constants.js'
import { debounce, deepClone } from '../utils/helpers.js'
import { apiPostJson } from '../utils/apiFetch.js'
import { trackEvents } from '../config/firebase.js'
import {
  applyGameLibraryOverrides,
  GAME_LIBRARY_SKILL_IDS,
  getDefaultEnabledGameLibrarySkillIds,
  getGameLibraryCapabilityIcon,
  getGameLibraryCapabilityLabel,
  getGameLibrarySelectableCapabilities,
  getGameResourceKey,
  normalizeGameLibrarySkillIds,
  rememberGameLibraryApp,
  runGameLibraryCuratorAgent,
} from '../utils/agents/gameLibrary/gameLibraryCuratorAgent.js'

const MESSAGE_DURATION = 3000
const GAME_LIBRARY_SKILL_PREFS_KEY = 'sunshine-game-library-skills:v1'
const REMOTE_IMAGE_URL_RE = /^https?:\/\//i
const COVER_LOCALIZATION_CONCURRENCY = 4
const COVER_LOCALIZATION_FAILED_FIELD = 'cover-localization-failed'

function getStorage() {
  return typeof localStorage !== 'undefined' ? localStorage : null
}

function loadEnabledGameLibrarySkillIds() {
  const storage = getStorage()
  if (!storage) {
    return getDefaultEnabledGameLibrarySkillIds()
  }

  try {
    const raw = storage.getItem(GAME_LIBRARY_SKILL_PREFS_KEY)
    if (!raw) {
      return getDefaultEnabledGameLibrarySkillIds()
    }
    const parsed = JSON.parse(raw)
    if (!Array.isArray(parsed?.enabledSkillIds)) {
      return getDefaultEnabledGameLibrarySkillIds()
    }
    return normalizeGameLibrarySkillIds(parsed.enabledSkillIds)
  } catch {
    return getDefaultEnabledGameLibrarySkillIds()
  }
}

function saveEnabledGameLibrarySkillIds(skillIds) {
  const storage = getStorage()
  if (!storage) return

  try {
    storage.setItem(
      GAME_LIBRARY_SKILL_PREFS_KEY,
      JSON.stringify({ enabledSkillIds: normalizeGameLibrarySkillIds(skillIds) })
    )
  } catch {
    // Skill preferences are convenience state; scanning should continue if persistence fails.
  }
}

async function mapWithConcurrency(items, concurrency, mapper) {
  const results = new Array(items.length)
  let nextIndex = 0
  const workerCount = Math.min(Math.max(1, concurrency), items.length)
  const workers = Array.from({ length: workerCount }, async () => {
    while (nextIndex < items.length) {
      const currentIndex = nextIndex
      nextIndex += 1
      results[currentIndex] = await mapper(items[currentIndex], currentIndex)
    }
  })

  await Promise.all(workers)
  return results
}

/**
 * 应用管理组合式函数
 */
export function useApps() {
  // 状态
  const apps = ref([])
  const originalApps = ref([])
  const searchQuery = ref('')
  const committedSearchQuery = ref('')
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
  const scannedEditSource = ref(null)
  const showScanResult = ref(false)
  const showScanOptions = ref(false)
  const scanProgress = reactive({
    active: false,
    stage: '',
    detail: '',
    current: 0,
    total: 0,
    indeterminate: false,
  })
  const scanOptions = reactive({
    scope: 'libraries',
    platforms: {
      steam: true,
      epic: true,
      gog: true,
    },
    extractIcons: true,
  })
  const enabledGameLibrarySkillIds = ref(loadEnabledGameLibrarySkillIds())
  const deleteConfirmIndex = ref(null)

  // 批量删除：selectionMode 仅控制 UI 是否显示多选 checkbox；
  // selectedIndices 是 Set<number>，存的是 apps.value 的原始 index，
  // 不能存 filteredApps 的下标，否则搜索后下标会错位。
  const selectionMode = ref(false)
  const selectedIndices = ref(new Set())
  const batchDeleteConfirm = ref(false)
  const isBatchDeleting = ref(false)
  const appRenderKeys = new WeakMap()
  let nextAppRenderKey = 0

  // 计算属性
  const messageClass = computed(() => ({
    [`alert-${messageType.value}`]: true,
  }))

  const normalizeAppForChangeCheck = (app) => {
    const { index, ...rest } = app
    return rest
  }

  const filteredApps = computed(() => AppService.searchApps(apps.value, committedSearchQuery.value))
  const appIndexByReference = computed(() => new Map(apps.value.map((app, index) => [app, index])))
  const filteredAppsWithIndex = computed(() =>
    filteredApps.value.map((app) => ({
      app,
      index: appIndexByReference.value.get(app) ?? -1,
    }))
  )
  const selectableGameLibrarySkills = computed(() => getGameLibrarySelectableCapabilities())
  const scanProgressPercent = computed(() => {
    if (!scanProgress.active || scanProgress.indeterminate || !scanProgress.total) return 0
    return Math.max(0, Math.min(100, Math.round((scanProgress.current / scanProgress.total) * 100)))
  })
  const scanPlatformOptions = [
    { id: 'steam', label: 'Steam' },
    { id: 'epic', label: 'Epic Games' },
    { id: 'gog', label: 'GOG' },
  ]

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

  let scanProgressHideTimer = null

  const setScanProgress = (next = {}) => {
    if (scanProgressHideTimer) {
      clearTimeout(scanProgressHideTimer)
      scanProgressHideTimer = null
    }

    Object.assign(scanProgress, {
      active: true,
      stage: next.stage || scanProgress.stage || '正在处理扫描结果',
      detail: next.detail || '',
      current: Number.isFinite(Number(next.current)) ? Number(next.current) : scanProgress.current,
      total: Number.isFinite(Number(next.total)) ? Number(next.total) : scanProgress.total,
      indeterminate: Boolean(next.indeterminate),
    })
  }

  const resetScanProgress = () => {
    if (scanProgressHideTimer) {
      clearTimeout(scanProgressHideTimer)
      scanProgressHideTimer = null
    }
    Object.assign(scanProgress, {
      active: false,
      stage: '',
      detail: '',
      current: 0,
      total: 0,
      indeterminate: false,
    })
  }

  const completeScanProgress = (detail = 'AI 增强完成') => {
    setScanProgress({
      stage: 'AI 增强完成',
      detail,
      current: scanProgress.total || scanProgress.current,
      total: scanProgress.total || scanProgress.current,
      indeterminate: false,
    })
    scanProgressHideTimer = setTimeout(resetScanProgress, 1800)
  }

  const hasRunnableScanEnhancement = (skillIds) => normalizeGameLibrarySkillIds(skillIds)
    .some((skillId) => skillId !== GAME_LIBRARY_SKILL_IDS.scanOverrideMemory)

  const updateScanEnhancementProgress = (progress = {}) => {
    const total = Number(progress.total) || 0
    const current = Math.min(Number(progress.current) || 0, total || Number(progress.current) || 0)

    if (progress.skillId === GAME_LIBRARY_SKILL_IDS.titleNormalize) {
      setScanProgress({
        stage: 'AI 正在清洗游戏名称',
        detail: progress.detail || progress.message || (total ? `正在处理第 ${current}/${total} 批` : '正在整理游戏名称和搜索关键词'),
        current,
        total,
        indeterminate: !total,
      })
      return
    }

    if (progress.skillId === GAME_LIBRARY_SKILL_IDS.coverSelection) {
      setScanProgress({
        stage: 'AI 正在匹配游戏封面',
        detail: progress.detail || progress.message || (total ? `已处理 ${current}/${total} 个游戏` : '正在搜索候选封面'),
        current,
        total,
        indeterminate: !total,
      })
      return
    }

    setScanProgress({
      stage: progress.stage || '正在处理扫描结果',
      detail: progress.detail || progress.message || '',
      current,
      total,
      indeterminate: progress.indeterminate ?? !total,
    })
  }

  const isGameLibrarySkillEnabled = (skillId) => enabledGameLibrarySkillIds.value.includes(skillId)

  const toggleGameLibrarySkill = (skillId) => {
    const selectable = selectableGameLibrarySkills.value.some((capability) => capability.skillId === skillId)
    if (!selectable) return

    const enabled = new Set(enabledGameLibrarySkillIds.value)
    if (enabled.has(skillId)) {
      enabled.delete(skillId)
    } else {
      enabled.add(skillId)
    }

    enabledGameLibrarySkillIds.value = normalizeGameLibrarySkillIds(Array.from(enabled))
    saveEnabledGameLibrarySkillIds(enabledGameLibrarySkillIds.value)
  }

  const getGameLibrarySkillIcon = (skillId) => getGameLibraryCapabilityIcon(skillId)

  const getGameLibrarySkillLabel = (skillId) => {
    const locale = typeof document === 'undefined'
      ? ''
      : String(document.documentElement?.getAttribute?.('lang') || '').toLowerCase()
    return getGameLibraryCapabilityLabel(skillId, { locale })
  }

  const openScanOptions = () => {
    showScanOptions.value = true
  }

  const closeScanOptions = () => {
    if (!isScanning.value) {
      showScanOptions.value = false
    }
  }

  const getSelectedScanPlatforms = () => scanPlatformOptions
    .filter((platformOption) => scanOptions.platforms[platformOption.id])
    .map((platformOption) => platformOption.id)

  const runConfiguredScan = async () => {
    if (scanOptions.scope === 'directory') {
      showScanOptions.value = false
      await scanDirectory(scanOptions.extractIcons)
      return
    }

    const platforms = getSelectedScanPlatforms()
    if (platforms.length === 0) {
      showMessage('请至少选择一个游戏平台', APP_CONSTANTS.MESSAGE_TYPES.WARNING)
      return
    }

    showScanOptions.value = false
    await scanGameLibraries({ platforms })
  }

  const getScanEnhancementMessage = (count, itemLabel) => {
    const titleEnabled = isGameLibrarySkillEnabled(GAME_LIBRARY_SKILL_IDS.titleNormalize)
    const coverEnabled = isGameLibrarySkillEnabled(GAME_LIBRARY_SKILL_IDS.coverSelection)

    if (titleEnabled && coverEnabled) {
      return `找到 ${count} 个${itemLabel}，正在清洗名称并搜索封面...`
    }
    if (titleEnabled) {
      return `找到 ${count} 个${itemLabel}，正在清洗名称...`
    }
    if (coverEnabled) {
      return `找到 ${count} 个${itemLabel}，正在搜索封面...`
    }
    return `找到 ${count} 个${itemLabel}`
  }

  const createDefaultApp = (overrides = {}) => ({
    ...APP_CONSTANTS.DEFAULT_APP,
    index: -1,
    ...overrides,
  })

  let translate = (key, params) => (params ? `${key} ${JSON.stringify(params)}` : key)

  // 初始化
  const init = (t) => {
    translate = t
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
    committedSearchQuery.value = searchQuery.value
  }

  const clearSearch = () => {
    searchQuery.value = ''
    committedSearchQuery.value = ''
    performSearch()
  }

  // 应用操作
  const getAppRenderKey = (app) => {
    const existingKey = appRenderKeys.get(app)
    if (existingKey) return existingKey

    const key = `app-${++nextAppRenderKey}`
    appRenderKeys.set(app, key)
    return key
  }

  const newApp = () => {
    trackEvents.userAction('new_app_clicked')
    scannedEditSource.value = null
    editingApp.value = createDefaultApp()
  }

  const editApp = (index) => {
    scannedEditSource.value = null
    editingApp.value = { ...deepClone(apps.value[index]), index }
  }

  const closeAppEditor = () => {
    scannedEditSource.value = null
    editingApp.value = null
  }

  const handleSaveApp = async (appData) => {
    try {
      isSaving.value = true

      await AppService.saveApps(apps.value, appData)
      if (scannedEditSource.value) {
        rememberGameLibraryApp(scannedEditSource.value, appData)
      }
      await loadApps()
      scannedEditSource.value = null
      editingApp.value = null
      showMessage('应用保存成功', APP_CONSTANTS.MESSAGE_TYPES.SUCCESS)
    } catch (error) {
      console.error('保存应用失败:', error)
      showMessage('保存应用失败', APP_CONSTANTS.MESSAGE_TYPES.ERROR)
    } finally {
      isSaving.value = false
    }
  }

  const showDeleteForm = (index) => {
    deleteConfirmIndex.value = index
  }

  const cancelDeleteApp = () => {
    deleteConfirmIndex.value = null
  }

  const confirmDeleteApp = async () => {
    const index = deleteConfirmIndex.value
    if (index === null) return
    deleteConfirmIndex.value = null
    await deleteApp(index)
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

  // 进入/退出多选模式，退出时清空选择
  const toggleSelectionMode = () => {
    selectionMode.value = !selectionMode.value
    if (!selectionMode.value) {
      selectedIndices.value = new Set()
    }
  }

  const toggleAppSelection = (index) => {
    const next = new Set(selectedIndices.value)
    if (next.has(index)) next.delete(index)
    else next.add(index)
    selectedIndices.value = next
  }

  const isAppSelected = (index) => selectedIndices.value.has(index)

  // 全选/反选
  const selectAllFiltered = () => {
    const next = new Set(selectedIndices.value)
    filteredAppsWithIndex.value.forEach(({ index }) => {
      if (index >= 0) next.add(index)
    })
    selectedIndices.value = next
  }

  const clearSelection = () => {
    selectedIndices.value = new Set()
  }

  const askBatchDelete = () => {
    if (selectedIndices.value.size === 0) return
    batchDeleteConfirm.value = true
  }

  const cancelBatchDelete = () => {
    batchDeleteConfirm.value = false
  }

  const confirmBatchDelete = async () => {
    const indices = Array.from(selectedIndices.value)
    if (indices.length === 0) {
      batchDeleteConfirm.value = false
      return
    }
    try {
      isBatchDeleting.value = true
      const result = await AppService.batchDeleteApps(indices)
      await loadApps()
      selectedIndices.value = new Set()
      selectionMode.value = false
      batchDeleteConfirm.value = false
      showMessage(
        translate('apps.batch_delete_result', { deleted: result.deleted, remaining: result.remaining }),
        APP_CONSTANTS.MESSAGE_TYPES.SUCCESS
      )
    } catch (error) {
      console.error('批量删除失败:', error)
      showMessage(
        error?.message || translate('apps.batch_delete_failed'),
        APP_CONSTANTS.MESSAGE_TYPES.ERROR
      )
    } finally {
      isBatchDeleting.value = false
    }
  }

  // 检测是否有未保存的更改
  const hasUnsavedChanges = computed(() => {
    if (apps.value.length !== originalApps.value.length) {
      return true
    }
  
    // 深度比较应用列表
    const appsStr = JSON.stringify(apps.value.map(normalizeAppForChangeCheck))
    const originalStr = JSON.stringify(originalApps.value.map(normalizeAppForChangeCheck))
    
    return appsStr !== originalStr
  })

  const save = async () => {
    // 如果没有更改，直接返回
    if (!hasUnsavedChanges.value) {
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
  const restoreDefaultApps = async () => {
    try {
      const result = AppService.restoreDefaultBuiltInApps(apps.value, platform.value)

      if (result.changed === 0) {
        showMessage(translate('apps.restore_default_apps_none'), APP_CONSTANTS.MESSAGE_TYPES.INFO)
        return
      }

      isSaving.value = true
      apps.value = result.apps
      await AppService.saveApps(apps.value, null)
      await loadApps()
      showMessage(
        translate('apps.restore_default_apps_result', { restored: result.restored, added: result.added }),
        APP_CONSTANTS.MESSAGE_TYPES.SUCCESS
      )
      trackEvents.userAction('default_apps_restored', {
        platform: platform.value,
        restored: result.restored,
        added: result.added,
      })
    } catch (error) {
      console.error('Restore default apps failed:', error)
      showMessage(translate('apps.restore_default_apps_failed'), APP_CONSTANTS.MESSAGE_TYPES.ERROR)
    } finally {
      isSaving.value = false
    }
  }

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

  let scanKeySequence = 0
  const withScanKeys = (appList) => {
    const scanId = scanKeySequence++
    return Array.isArray(appList)
      ? appList.map((app, index) => ({
          ...app,
          '__scan-key': app['__scan-key'] || `scan-${scanId}-${index}`,
        }))
      : appList
  }

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
        const overriddenApps = withScanKeys(applyGameLibraryOverrides(foundApps))
        scannedApps.value = overriddenApps
        showScanResult.value = true
        showMessage(getScanEnhancementMessage(foundApps.length, '应用程序'), APP_CONSTANTS.MESSAGE_TYPES.INFO)

        // 异步更新封面图片
        asyncEnhanceAndUpdateCovers(overriddenApps, enabledGameLibrarySkillIds.value)
      }

      trackEvents.userAction('directory_scanned', { count: foundApps.length, extractIcons })
    } catch (error) {
      console.error('扫描目录失败:', error)
      showMessage(`扫描失败: ${error}`, APP_CONSTANTS.MESSAGE_TYPES.ERROR)
    } finally {
      isScanning.value = false
    }
  }

  // 扫描游戏平台库（Steam/Epic/GOG）
  const scanGameLibraries = async (options = {}) => {
    const tauri = window.__TAURI__
    if (!tauri?.core?.invoke) {
      showMessage('扫描功能仅在 Tauri 环境下可用', APP_CONSTANTS.MESSAGE_TYPES.WARNING)
      return
    }

    try {
      isScanning.value = true
      showMessage('正在扫描游戏平台库...', APP_CONSTANTS.MESSAGE_TYPES.INFO)

      const requestedPlatforms = options.platforms || scanPlatformOptions.map((platformOption) => platformOption.id)
      const result = await tauri.core.invoke('scan_game_libraries', {
        platforms: requestedPlatforms,
      })

      // 将 PlatformGame 转换为 scannedApps 格式
      const selectedPlatforms = new Set(requestedPlatforms)
      const steamGames = selectedPlatforms.has('steam') ? result.steam || [] : []
      const epicGames = selectedPlatforms.has('epic') ? result.epic || [] : []
      const gogGames = selectedPlatforms.has('gog') ? result.gog || [] : []
      const allGames = [...steamGames, ...epicGames, ...gogGames]

      if (allGames.length === 0) {
        scannedApps.value = []
        showScanResult.value = true
        showMessage('未检测到已安装的游戏', APP_CONSTANTS.MESSAGE_TYPES.INFO)
      } else {
        const mapped = allGames.map((game) => ({
          name: game.name,
          cmd: game.cmd,
          'working-dir': game['working-dir'] || game.working_dir || '',
          'image-path': game['cover-url'] || game.cover_url || '',
          source_path: game.install_dir,
          'app-type': game.platform,
          'is-game': true,
        }))

        const overriddenApps = withScanKeys(applyGameLibraryOverrides(mapped))
        scannedApps.value = overriddenApps
        showScanResult.value = true

        const parts = []
        if (steamGames.length) parts.push(`Steam ${steamGames.length}`)
        if (epicGames.length) parts.push(`Epic ${epicGames.length}`)
        if (gogGames.length) parts.push(`GOG ${gogGames.length}`)
        showMessage(
          `找到 ${result.total ?? allGames.length} 个游戏 (${parts.join(', ')})，耗时 ${result.scan_time_ms ?? 0}ms`,
          APP_CONSTANTS.MESSAGE_TYPES.SUCCESS
        )
        asyncEnhanceAndUpdateCovers(overriddenApps, enabledGameLibrarySkillIds.value)
      }

      trackEvents.userAction('game_libraries_scanned', {
        steam: steamGames.length,
        epic: epicGames.length,
        gog: gogGames.length,
        total: result.total ?? allGames.length,
      })
    } catch (error) {
      console.error('扫描游戏库失败:', error)
      showMessage(`扫描游戏库失败: ${error}`, APP_CONSTANTS.MESSAGE_TYPES.ERROR)
    } finally {
      isScanning.value = false
    }
  }

  // 异步更新封面图片
  const getScannedAppKey = getGameResourceKey

  const applyEnhancedScannedApps = (baseList, enhancedList) => {
    const enhancedByKey = new Map(baseList.map((app, index) => [getScannedAppKey(app, index), enhancedList[index]]))
    let changed = 0

    scannedApps.value = scannedApps.value.map((current, index) => {
      const next = enhancedByKey.get(getScannedAppKey(current, index))
      if (!next) return current

      if (next.name !== current.name || next['canonical-name'] !== current['canonical-name']) {
        changed++
      }

      return { ...current, ...next }
    })

    return changed
  }

  const asyncEnhanceAndUpdateCovers = async (appList, enabledSkillIds = enabledGameLibrarySkillIds.value) => {
    const enabled = normalizeGameLibrarySkillIds(enabledSkillIds)
    const shouldRunEnhancement = hasRunnableScanEnhancement(enabled)
    let result

    if (!shouldRunEnhancement) {
      resetScanProgress()
      return
    }

    setScanProgress({
      stage: '准备 AI 增强',
      detail: `将处理 ${appList.length} 个扫描结果`,
      current: 0,
      total: appList.length,
      indeterminate: true,
    })

    try {
      result = await runGameLibraryCuratorAgent(appList, {
        enabledSkills: enabled,
        onSkillProgress: updateScanEnhancementProgress,
        onTitlesEnhanced(enhanced, { changed }) {
          applyEnhancedScannedApps(appList, enhanced)
          if (changed > 0) {
            showMessage(`AI 已清洗 ${changed} 个游戏名称`, APP_CONSTANTS.MESSAGE_TYPES.SUCCESS)
          }
        },
        onCoverResolved(next, { key }) {
          const currentIndex = scannedApps.value.findIndex((current, currentIndex) => getScannedAppKey(current, currentIndex) === key)
          if (currentIndex !== -1) {
            scannedApps.value[currentIndex] = {
              ...scannedApps.value[currentIndex],
              ...next,
            }
          }
        },
        onSkillError(skillId, error) {
          if (skillId === GAME_LIBRARY_SKILL_IDS.titleNormalize) {
            console.warn('AI name cleanup failed; falling back to original names:', error)
            showMessage('AI 名称清洗不可用，已回退到原始名称搜索', APP_CONSTANTS.MESSAGE_TYPES.INFO)
          } else if (skillId === GAME_LIBRARY_SKILL_IDS.coverSelection) {
            console.warn('AI cover selection failed:', error)
          }
        },
      })
    } catch (error) {
      console.warn('Game library enrichment failed:', error)
      showMessage('游戏资源增强不可用，已保留原始扫描结果', APP_CONSTANTS.MESSAGE_TYPES.INFO)
      completeScanProgress('AI 增强不可用，已保留扫描结果')
      return
    }

    if (enabled.includes(GAME_LIBRARY_SKILL_IDS.coverSelection)) {
      const coversFound = result.stats?.coversFound || 0
      const total = appList.length
      showMessage(
        `已匹配 ${coversFound}/${total} 个封面`,
        coversFound > 0 ? APP_CONSTANTS.MESSAGE_TYPES.SUCCESS : APP_CONSTANTS.MESSAGE_TYPES.INFO
      )
    }

    completeScanProgress('扫描结果已更新')
  }

  // 扫描应用字段处理
  const getScannedAppField = (app, field) => app[field] || app[field.replace(/-/g, '_')] || ''

  const localizeScannedAppCover = async (scannedApp) => {
    const imagePath = getScannedAppField(scannedApp, 'image-path')
    if (!REMOTE_IMAGE_URL_RE.test(imagePath)) {
      return scannedApp
    }

    try {
      const result = await apiPostJson('/api/covers/upload', {
        key: scannedApp.name,
        url: imagePath,
      })

      if (result?.path) {
        return {
          ...scannedApp,
          'image-path': result.path,
          image_path: result.path,
          'cover-localized': true,
        }
      }
      console.warn('Cover localization did not return a local path:', scannedApp.name, result)
    } catch (error) {
      console.warn('Failed to localize scanned cover:', scannedApp.name, error)
    }

    return {
      ...scannedApp,
      [COVER_LOCALIZATION_FAILED_FIELD]: true,
    }
  }

  const createAppFromScanned = (scannedApp) => ({
    ...APP_CONSTANTS.DEFAULT_APP,
    name: scannedApp.name,
    cmd: scannedApp.cmd,
    'working-dir': getScannedAppField(scannedApp, 'working-dir'),
    'image-path': getScannedAppField(scannedApp, 'image-path'),
  })

  const didCoverLocalizationFail = (scannedApp) => scannedApp?.[COVER_LOCALIZATION_FAILED_FIELD] === true

  const findScannedAppIndex = (scannedAppOrIndex) => {
    if (Number.isInteger(scannedAppOrIndex)) {
      return scannedAppOrIndex
    }

    let index = scannedApps.value.indexOf(scannedAppOrIndex)
    if (index === -1 && scannedAppOrIndex?.source_path) {
      index = scannedApps.value.findIndex((app) => app.source_path === scannedAppOrIndex.source_path)
    }
    return index
  }

  const removeScannedApp = (scannedAppOrIndex) => {
    const index = findScannedAppIndex(scannedAppOrIndex)
    if (index >= 0 && index < scannedApps.value.length) {
      scannedApps.value.splice(index, 1)
      if (scannedApps.value.length === 0) {
        showScanResult.value = false
      }
    }
  }

  const showCoverLocalizationMessage = (localizedApp, successMessage, successType) => {
    const failed = didCoverLocalizationFail(localizedApp)
    showMessage(
      failed ? `${successMessage}，但封面本地化失败，已保留原始封面地址` : successMessage,
      failed ? APP_CONSTANTS.MESSAGE_TYPES.WARNING : successType
    )
  }

  const addScannedApp = async (scannedApp) => {
    const localizedApp = await localizeScannedAppCover(scannedApp)

    editingApp.value = createDefaultApp({
      name: localizedApp.name,
      cmd: localizedApp.cmd,
      'working-dir': getScannedAppField(localizedApp, 'working-dir'),
      'image-path': getScannedAppField(localizedApp, 'image-path'),
    })
    scannedEditSource.value = { ...localizedApp }

    removeScannedApp(scannedApp)
    showCoverLocalizationMessage(localizedApp, `正在编辑应用: ${scannedApp.name}`, APP_CONSTANTS.MESSAGE_TYPES.INFO)
    trackEvents.userAction('scanned_app_edit', { name: scannedApp.name })
  }

  const quickAddScannedApp = async (scannedApp) => {
    try {
      const localizedApp = await localizeScannedAppCover(scannedApp)
      const appToAdd = createAppFromScanned(localizedApp)

      apps.value.push(appToAdd)
      await AppService.saveApps(apps.value, null)
      rememberGameLibraryApp(scannedApp, appToAdd)
      await loadApps()

      removeScannedApp(scannedApp)

      showCoverLocalizationMessage(localizedApp, `已添加应用: ${scannedApp.name}`, APP_CONSTANTS.MESSAGE_TYPES.SUCCESS)
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
      const scannedAppSnapshot = [...scannedApps.value]
      const localizedScannedApps = await mapWithConcurrency(
        scannedAppSnapshot,
        COVER_LOCALIZATION_CONCURRENCY,
        localizeScannedAppCover
      )
      const appsToAdd = localizedScannedApps.map(createAppFromScanned)

      apps.value.push(...appsToAdd)
      await AppService.saveApps(apps.value, null)
      scannedAppSnapshot.forEach((scannedApp, index) => rememberGameLibraryApp(scannedApp, appsToAdd[index]))
      await loadApps()

      const coverLocalizationFailureCount = localizedScannedApps.filter(didCoverLocalizationFail).length
      showMessage(
        coverLocalizationFailureCount > 0
          ? `已添加 ${appsToAdd.length} 个应用，其中 ${coverLocalizationFailureCount} 个封面本地化失败，已保留原始封面地址`
          : `已添加 ${appsToAdd.length} 个应用`,
        coverLocalizationFailureCount > 0 ? APP_CONSTANTS.MESSAGE_TYPES.WARNING : APP_CONSTANTS.MESSAGE_TYPES.SUCCESS
      )
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
  }

  const handleCopySuccess = () => showMessage('复制成功', APP_CONSTANTS.MESSAGE_TYPES.SUCCESS)
  const handleCopyError = () => showMessage('复制失败', APP_CONSTANTS.MESSAGE_TYPES.ERROR)

  return {
    // 状态
    apps,
    filteredApps,
    filteredAppsWithIndex,
    searchQuery,
    editingApp,
    platform,
    isSaving,
    isDragging,
    viewMode,
    message,
    envVars,
    debouncedSearch,
    isScanning,
    scannedApps,
    showScanResult,
    showScanOptions,
    scanProgress,
    scanOptions,
    scanPlatformOptions,
    selectableGameLibrarySkills,
    selectionMode,
    selectedIndices,
    batchDeleteConfirm,
    isBatchDeleting,
    // 计算属性
    messageClass,
    scanProgressPercent,
    // 方法
    init,
    loadApps,
    loadPlatform,
    clearSearch,
    getAppRenderKey,
    newApp,
    editApp,
    closeAppEditor,
    handleSaveApp,
    showDeleteForm,
    cancelDeleteApp,
    confirmDeleteApp,
    deleteConfirmIndex,
    toggleSelectionMode,
    toggleAppSelection,
    isAppSelected,
    selectAllFiltered,
    clearSelection,
    askBatchDelete,
    cancelBatchDelete,
    confirmBatchDelete,
    save,
    restoreDefaultApps,
    hasUnsavedChanges,
    onDragStart,
    onDragEnd,
    openScanOptions,
    closeScanOptions,
    runConfiguredScan,
    addScannedApp,
    quickAddScannedApp,
    addAllScannedApps,
    closeScanResult,
    removeScannedApp,
    isTauriEnv,
    isGameLibrarySkillEnabled,
    toggleGameLibrarySkill,
    getGameLibrarySkillIcon,
    getGameLibrarySkillLabel,
    getMessageIcon,
    handleCopySuccess,
    handleCopyError,
  }
}
