import { ref } from 'vue'
import { trackEvents } from '../config/firebase.js'
import { getBootstrapConfig } from '../config/bootstrapData.js'
import { apiFetch } from '../utils/apiFetch.js'
import { deepClone, safeJsonParse } from '../utils/helpers.js'

// 平台相关的标签页排除规则
const PLATFORM_EXCLUSIONS = {
  windows: ['vt', 'vaapi'],
  linux: ['amd', 'qsv', 'vt'],
  macos: ['amd', 'nv', 'qsv', 'vaapi'],
}

// 不参与默认值比较的键
const EXCLUDED_DEFAULT_KEYS = new Set(['resolutions', 'fps', 'adapter_name'])

// 默认标签页配置
const DEFAULT_TABS = [
  {
    id: 'general',
    name: 'General',
    options: {
      locale: 'en',
      sunshine_name: '',
      min_log_level: 2,
      global_prep_cmd: '[]',
      notify_pre_releases: 'disabled',
    },
  },
  {
    id: 'input',
    name: 'Input',
    options: {
      controller: 'enabled',
      gamepad: 'auto',
      ds4_back_as_touchpad_click: 'enabled',
      motion_as_ds4: 'enabled',
      touchpad_as_ds4: 'enabled',
      back_button_timeout: -1,
      keyboard: 'enabled',
      key_repeat_delay: 500,
      key_repeat_frequency: 24.9,
      always_send_scancodes: 'enabled',
      key_rightalt_to_key_win: 'disabled',
      mouse: 'enabled',
      high_resolution_scrolling: 'enabled',
      native_pen_touch: 'enabled',
      native_touchpad_optimization: 'enabled',
      virtual_mouse: 'enabled',
      capture_cursor: true,
      amf_draw_mouse_cursor: false,
      enable_dsu_server: 'disabled',
      dsu_server_port: 26760,
      keybindings: '[0x10,0xA0,0x11,0xA2,0x12,0xA4]',
    },
  },
  {
    id: 'av',
    name: 'Audio/Video',
    options: {
      audio_sink: '',
      virtual_sink: '',
      install_steam_audio_drivers: 'enabled',
      adapter_name: '',
      output_name: '',
      capture_target: 'display',
      capture_compute_shader: 'auto',
      vdd_borrowed_texture: 'enabled',
      vdd_vulkan_hdr_bridge: 'enabled',
      window_title: '',
      display_device_prep: 'no_operation',
      vdd_reuse: 'disabled',
      resolution_change: 'automatic',
      manual_resolution: '',
      refresh_rate_change: 'automatic',
      manual_refresh_rate: '',
      hdr_prep: 'automatic',
      display_mode_remapping: '[]',
      resolutions: '[1280x720,1920x1080,2560x1080,2560x1440,2560x1600,3440x1440,3840x2160]',
      fps: '[60,90,120,144]',
      max_bitrate: 0,
      variable_refresh_rate: 'disabled',
      minimum_fps_target: 0,
    },
  },
  {
    id: 'network',
    name: 'Network',
    options: {
      upnp: 'disabled',
      address_family: 'ipv4',
      port: 47989,
      origin_web_ui_allowed: 'lan',
      external_ip: '',
      lan_encryption_mode: 0,
      wan_encryption_mode: 1,
      close_verify_safe: 'disabled',
      mdns_broadcast: 'enabled',
      ping_timeout: 10000,
      webhook_url: '',
      webhook_enabled: 'disabled',
      webhook_skip_ssl_verify: 'disabled',
      webhook_timeout: 1000,
      pair_max_attempts: 10,
    },
  },
  {
    id: 'files',
    name: 'Config Files',
    options: {
      file_apps: '',
      credentials_file: '',
      log_path: '',
      pkey: '',
      cert: '',
      file_state: '',
    },
  },
  {
    id: 'advanced',
    name: 'Advanced',
    options: {
      fec_percentage: 20,
      qp: 28,
      min_threads: 2,
      hevc_mode: 0,
      av1_mode: 0,
      capture: '',
      encoder: '',
    },
  },
  {
    id: 'encoders',
    name: 'Encoders',
    type: 'group',
    children: [
      {
        id: 'nv',
        name: 'NVIDIA NVENC Encoder',
        options: {
          nvenc_preset: 1,
          nvenc_twopass: 'quarter_res',
          nvenc_spatial_aq: 'disabled',
          nvenc_temporal_aq: 'disabled',
          nvenc_vbv_increase: 0,
          nvenc_lookahead_depth: 0,
          nvenc_lookahead_level: 'disabled',
          nvenc_temporal_filter: 'disabled',
          nvenc_rate_control: 'cbr',
          nvenc_target_quality: 0,
          nvenc_realtime_hags: 'enabled',
          nvenc_split_encode: 'driver_decides',
          nvenc_latency_over_power: 'enabled',
          nvenc_opengl_vulkan_on_dxgi: 'enabled',
          nvenc_h264_cavlc: 'disabled',
        },
      },
      {
        id: 'qsv',
        name: 'Intel QuickSync Encoder',
        options: {
          qsv_preset: 'medium',
          qsv_coder: 'auto',
          qsv_slow_hevc: 'disabled',
        },
      },
      {
        id: 'amd',
        name: 'AMD AMF Encoder',
        options: {
          amd_usage: '',
          amd_rc: '',
          amd_enforce_hrd: '',
          amd_quality: '',
          amd_preanalysis: '',
          amd_vbaq: '',
          amd_coder: 'auto',
          amd_avcodec_compat: 'disabled',
          // AMF advanced (driver workarounds): empty string = driver default
          // (FFmpeg-aligned). Users can override per-property if troubleshooting
          // freezes or tuning latency. See issue #666 (RDNA4 26.5.x).
          amd_high_motion_qb: '',
          amd_lowlatency_mode: '',
          amd_multi_hw_instance: '',
          amd_input_queue_size: '',
          amd_av1_latency_mode: '',
        },
      },
      {
        id: 'vt',
        name: 'VideoToolbox Encoder',
        options: {
          vt_coder: 'auto',
          vt_software: 'auto',
          vt_realtime: 'enabled',
        },
      },
      {
        id: 'sw',
        name: 'Software Encoder',
        options: {
          sw_preset: 'superfast',
          sw_tune: 'zerolatency',
        },
      },
    ],
  },
]

/**
 * 安全解析 JSON
 */
const safeParseJSON = (str, fallback = []) => {
  if (!str) return fallback
  return safeJsonParse(str, fallback)
}

/**
 * 判断是否应该删除默认值
 */
const shouldDeleteDefault = (configData, tab, optionKey) => {
  if (EXCLUDED_DEFAULT_KEYS.has(optionKey)) return false

  const currentValue = configData[optionKey]
  const defaultValue = tab.options[optionKey]

  try {
    return JSON.stringify(JSON.parse(currentValue)) === JSON.stringify(JSON.parse(defaultValue))
  } catch {
    return String(currentValue) === String(defaultValue)
  }
}

/**
 * 遍历所有标签页选项
 */
const forEachTabOption = (tabs, callback) => {
  for (const tab of tabs) {
    if (tab.type === 'group' && tab.children) {
      for (const childTab of tab.children) {
        callback(childTab)
      }
    } else if (tab.options) {
      callback(tab)
    }
  }
}

/**
 * 序列化分辨率数组
 */
export const serializeResolutions = (resolutions) =>
  JSON.stringify(resolutions).replace(/","/g, ',').replace(/^\["/, '[').replace(/"\]$/, ']')

/**
 * 序列化 FPS 数组
 */
export const serializeFps = (fps) => JSON.stringify(fps).replace(/"/g, '')

/**
 * 解析分辨率字符串
 */
export const parseResolutions = (resStr) => {
  try {
    return JSON.parse((resStr || '').replace(/(\d+)x(\d+)/g, '"$1x$2"'))
  } catch {
    return []
  }
}

/**
 * 过滤有效的 FPS 值
 */
export const filterValidFps = (fps) => fps.filter((item) => +item >= 30 && +item <= 500)

export const normalizeEnabledDisabledValue = (value) => {
  if (value === undefined || value === null || value === '') return value

  const normalized = String(value).trim().toLowerCase()
  if (['true', 'yes', 'enable', 'enabled', 'on', '1'].includes(normalized)) return 'enabled'
  if (['false', 'no', 'disable', 'disabled', 'off', '0'].includes(normalized)) return 'disabled'

  return value
}

const RISK_SEVERITY_WEIGHT = {
  critical: 3,
  high: 2,
  medium: 1,
}

const isEnabledValue = (value) => value === true || value === 'true' || value === 'enabled' || value === 1 || value === '1'

const hasCommandText = (cmd) => {
  if (!cmd) return false
  if (typeof cmd === 'string') return cmd.trim().length > 0
  return Boolean(String(cmd.do || cmd.cmd || cmd.undo || '').trim())
}

const hasElevatedCommand = (commands) => commands.some((cmd) => isEnabledValue(cmd?.elevated))

const compactValue = (value) => {
  if (Array.isArray(value)) return `${value.length}`
  if (value === undefined || value === null || value === '') return ''
  return String(value)
}

const riskLocaleKey = (id, field) => `config.risk_confirm.items.${id}.${field}`

const createRisk = (
  id,
  severity,
  { descriptionField = 'description', recoveryField = 'recovery', recoveryId = id, currentValue } = {},
) => {
  const risk = {
    id,
    severity,
    titleKey: riskLocaleKey(id, 'title'),
    descriptionKey: riskLocaleKey(id, descriptionField),
  }

  if (recoveryField) {
    risk.recoveryKey = riskLocaleKey(recoveryId, recoveryField)
  }

  if (currentValue !== undefined && currentValue !== null && currentValue !== '') {
    risk.currentValue = currentValue
  }

  return risk
}

/**
 * 配置管理组合式函数
 */
export function useConfig() {
  const platform = ref('')
  const saved = ref(false)
  const restarted = ref(false)
  const config = ref(null)
  const fps = ref([])
  const resolutions = ref([])
  const currentTab = ref('general')
  const global_prep_cmd = ref([])
  const display_mode_remapping = ref([])
  const tabs = ref([])

  // 原始配置快照
  const snapshots = ref({
    config: null,
    fps: null,
    resolutions: null,
    global_prep_cmd: null,
    display_mode_remapping: null,
  })

  /**
   * 保存当前状态快照
   */
  const saveSnapshots = () => {
    snapshots.value = {
      config: deepClone(config.value),
      fps: deepClone(fps.value),
      resolutions: deepClone(resolutions.value),
      global_prep_cmd: deepClone(global_prep_cmd.value),
      display_mode_remapping: deepClone(display_mode_remapping.value),
    }
  }

  /**
   * 初始化标签页配置
   */
  const initTabs = () => {
    tabs.value = deepClone(DEFAULT_TABS)
  }

  /**
   * 根据平台过滤标签页
   */
  const filterTabsByPlatform = (platformName) => {
    const exclusions = PLATFORM_EXCLUSIONS[platformName] || []

    tabs.value = tabs.value
      .map((tab) => {
        if (tab.type === 'group' && tab.children) {
          const filteredChildren = tab.children.filter((child) => !exclusions.includes(child.id))
          return filteredChildren.length > 0 ? { ...tab, children: filteredChildren } : null
        }
        return exclusions.includes(tab.id) ? null : tab
      })
      .filter(Boolean)
  }

  /**
   * 填充配置默认值
   */
  const fillDefaultValues = () => {
    forEachTabOption(tabs.value, (tab) => {
      for (const [key, defaultVal] of Object.entries(tab.options)) {
        if (config.value[key] === undefined) {
          config.value[key] = defaultVal
        }
      }
    })
  }

  /**
   * 解析特殊字段
   */
  const parseSpecialFields = () => {
    fps.value = safeParseJSON(config.value.fps)
    resolutions.value = parseResolutions(config.value.resolutions)
    global_prep_cmd.value = safeParseJSON(config.value.global_prep_cmd)
    display_mode_remapping.value = safeParseJSON(config.value.display_mode_remapping)

    config.value.global_prep_cmd = config.value.global_prep_cmd || []
    config.value.display_mode_remapping = config.value.display_mode_remapping || []
  }

  /**
   * 加载配置
   */
  const loadConfig = async () => {
    try {
      const data = await getBootstrapConfig()

      platform.value = data.platform || ''
      filterTabsByPlatform(platform.value)

      const { platform: _, status, version, ...configData } = data
      configData.amd_avcodec_compat = normalizeEnabledDisabledValue(configData.amd_avcodec_compat)
      config.value = configData

      fillDefaultValues()
      parseSpecialFields()
      saveSnapshots()
    } catch (error) {
      console.error('Failed to load config:', error)
    }
  }

  /**
   * 序列化配置
   */
  const serialize = () => {
    config.value.resolutions = serializeResolutions(resolutions.value)
    fps.value = filterValidFps(fps.value)
    config.value.fps = serializeFps(fps.value)
    config.value.global_prep_cmd = JSON.stringify(global_prep_cmd.value)
    config.value.display_mode_remapping = JSON.stringify(display_mode_remapping.value)
  }

  /**
   * 构建当前配置的序列化快照，不修改响应式状态。
   */
  const buildCurrentSnapshot = () => {
    const currentConfig = deepClone(config.value || {})
    currentConfig.resolutions = serializeResolutions(resolutions.value)
    currentConfig.fps = serializeFps(filterValidFps([...fps.value]))
    currentConfig.global_prep_cmd = JSON.stringify(global_prep_cmd.value)
    currentConfig.display_mode_remapping = JSON.stringify(display_mode_remapping.value)

    return {
      config: currentConfig,
      global_prep_cmd: deepClone(global_prep_cmd.value),
      display_mode_remapping: deepClone(display_mode_remapping.value),
    }
  }

  const addRisk = (risks, risk) => {
    if (risks.some((item) => item.id === risk.id)) return
    risks.push(risk)
  }

  const valueChanged = (currentConfig, key) => !isEqual(currentConfig[key], snapshots.value.config?.[key])

  const listChanged = (currentList, originalList) =>
    !isEqual(JSON.stringify(currentList), JSON.stringify(originalList || []))

  /**
   * 返回保存/应用前需要用户二次确认的风险项。
   */
  const getRiskyChanges = (action = 'save') => {
    if (!config.value || !snapshots.value.config) {
      return []
    }

    const current = buildCurrentSnapshot()
    const currentConfig = current.config
    const risks = []

    if (action === 'apply') {
      addRisk(risks, createRisk('restart', 'high'))
    }

    if (valueChanged(currentConfig, 'origin_web_ui_allowed') && currentConfig.origin_web_ui_allowed === 'wan') {
      addRisk(risks, createRisk('web_ui_wan', 'critical', { currentValue: currentConfig.origin_web_ui_allowed }))
    }

    if (valueChanged(currentConfig, 'upnp') && isEnabledValue(currentConfig.upnp)) {
      addRisk(risks, createRisk('upnp_enabled', 'high', { currentValue: currentConfig.upnp }))
    }

    if (valueChanged(currentConfig, 'wan_encryption_mode') && String(currentConfig.wan_encryption_mode) === '0') {
      addRisk(
        risks,
        createRisk('wan_encryption_disabled', 'critical', { currentValue: currentConfig.wan_encryption_mode }),
      )
    }

    if (
      valueChanged(currentConfig, 'webhook_skip_ssl_verify') &&
      isEnabledValue(currentConfig.webhook_skip_ssl_verify)
    ) {
      addRisk(
        risks,
        createRisk('webhook_skip_ssl_verify', 'high', { currentValue: currentConfig.webhook_skip_ssl_verify }),
      )
    }

    if (
      (valueChanged(currentConfig, 'webhook_enabled') || valueChanged(currentConfig, 'webhook_url')) &&
      isEnabledValue(currentConfig.webhook_enabled) &&
      currentConfig.webhook_url
    ) {
      addRisk(risks, createRisk('webhook_enabled', 'medium', { currentValue: currentConfig.webhook_url }))
    }

    if (valueChanged(currentConfig, 'pair_max_attempts') && Number(currentConfig.pair_max_attempts) === 0) {
      addRisk(risks, createRisk('pair_limit_disabled', 'high', { currentValue: currentConfig.pair_max_attempts }))
    }

    if (listChanged(current.global_prep_cmd, snapshots.value.global_prep_cmd)) {
      const commands = current.global_prep_cmd.filter(hasCommandText)
      if (commands.length > 0) {
        const elevated = hasElevatedCommand(commands)
        addRisk(
          risks,
          createRisk(elevated ? 'elevated_global_commands' : 'global_prep_commands', elevated ? 'critical' : 'high', {
            recoveryId: 'global_prep_commands',
            currentValue: String(commands.length),
          }),
        )
      }
    }

    const sensitiveFileKeys = ['credentials_file', 'pkey', 'cert', 'file_state']
    const changedSensitiveFiles = sensitiveFileKeys.filter((key) => valueChanged(currentConfig, key))
    if (changedSensitiveFiles.length > 0) {
      addRisk(risks, createRisk('sensitive_files_changed', 'high', { currentValue: changedSensitiveFiles.join(', ') }))
    }

    if (
      valueChanged(currentConfig, 'display_device_prep') &&
      currentConfig.display_device_prep &&
      currentConfig.display_device_prep !== 'no_operation'
    ) {
      const onlyDisplay = currentConfig.display_device_prep === 'ensure_only_display'
      addRisk(
        risks,
        createRisk('display_device_prep', onlyDisplay ? 'critical' : 'high', {
          descriptionField: onlyDisplay ? 'only_display_description' : 'description',
          currentValue: currentConfig.display_device_prep,
        }),
      )
    }

    const displayModeKeys = ['resolution_change', 'manual_resolution', 'refresh_rate_change', 'manual_refresh_rate']
    if (displayModeKeys.some((key) => valueChanged(currentConfig, key))) {
      const resolutionActive = currentConfig.resolution_change && currentConfig.resolution_change !== 'no_operation'
      const refreshActive = currentConfig.refresh_rate_change && currentConfig.refresh_rate_change !== 'no_operation'

      if (resolutionActive || refreshActive) {
        addRisk(
          risks,
          createRisk('display_mode_change', 'medium', {
            currentValue: [
              compactValue(currentConfig.resolution_change),
              compactValue(currentConfig.manual_resolution),
              compactValue(currentConfig.refresh_rate_change),
              compactValue(currentConfig.manual_refresh_rate),
            ].filter(Boolean).join(' / '),
          }),
        )
      }
    }

    if (
      listChanged(current.display_mode_remapping, snapshots.value.display_mode_remapping) &&
      current.display_mode_remapping.length > 0
    ) {
      addRisk(
        risks,
        createRisk('display_mode_remapping', 'medium', {
          currentValue: String(current.display_mode_remapping.length),
        }),
      )
    }

    if (
      valueChanged(currentConfig, 'wgc_disable_secure_desktop') &&
      isEnabledValue(currentConfig.wgc_disable_secure_desktop)
    ) {
      addRisk(
        risks,
        createRisk('wgc_disable_secure_desktop', 'high', {
          currentValue: compactValue(currentConfig.wgc_disable_secure_desktop),
        }),
      )
    }

    if (valueChanged(currentConfig, 'capture') && ['amd'].includes(currentConfig.capture)) {
      addRisk(risks, createRisk('experimental_capture', 'medium', { currentValue: currentConfig.capture }))
    }

    return risks.sort((a, b) => RISK_SEVERITY_WEIGHT[b.severity] - RISK_SEVERITY_WEIGHT[a.severity])
  }

  /**
   * 移除默认值
   */
  const removeDefaultValues = (configData) => {
    forEachTabOption(tabs.value, (tab) => {
      for (const optionKey of Object.keys(tab.options)) {
        if (shouldDeleteDefault(configData, tab, optionKey)) {
          delete configData[optionKey]
        }
      }
    })
  }

  /**
   * 保存配置
   */
  const save = async () => {
    saved.value = false
    restarted.value = false
    serialize()

    const configData = deepClone(config.value)
    removeDefaultValues(configData)

    try {
      const response = await apiFetch('/api/config', {
        method: 'POST',
        body: configData,
      })

      saved.value = response.ok

      if (saved.value) {
        trackEvents.configChanged(currentTab.value, 'save')
        saveSnapshots()
      }

      return saved.value
    } catch (error) {
      console.error('Save failed:', error)
      trackEvents.errorOccurred('config_save', error.message)
      return false
    }
  }

  /**
   * 应用配置（保存并重启）
   */
  const apply = async () => {
    saved.value = false
    restarted.value = false

    const result = await save()
    if (!result) return

    restarted.value = true
    setTimeout(() => {
      saved.value = false
      restarted.value = false
    }, 5000)

    try {
      await apiFetch('/api/restart', { method: 'POST' })
      trackEvents.userAction('config_applied')
    } catch (error) {
      console.error('Failed to restart:', error)
    }
  }

  /**
   * 在标签页中查找目标
   */
  const findTabByHash = (hash) => {
    for (const tab of tabs.value) {
      if (tab.id === hash || (tab.options && Object.keys(tab.options).includes(hash))) {
        return tab
      }

      if (tab.type === 'group' && tab.children) {
        const childTab = tab.children.find(
          (child) => child.id === hash || Object.keys(child.options).includes(hash)
        )
        if (childTab) return childTab
      }
    }
    return null
  }

  /**
   * 处理哈希导航
   */
  const handleHash = () => {
    const hash = window.location.hash.slice(1)
    if (!hash) return

    const targetTab = findTabByHash(hash)
    if (targetTab) {
      currentTab.value = targetTab.id
      setTimeout(() => {
        document.getElementById(hash)?.scrollIntoView({ behavior: 'smooth' })
      }, 100)
    }
  }

  /**
   * 比较两个值是否相等
   */
  const isEqual = (a, b) => {
    if (a === b) return true
    if (a === undefined || b === undefined) return false

    try {
      return JSON.stringify(JSON.parse(a)) === JSON.stringify(JSON.parse(b))
    } catch {
      return String(a) === String(b)
    }
  }

  /**
   * 比较两个配置对象
   */
  const configsAreEqual = (current, original) => {
    const allKeys = new Set([...Object.keys(current), ...Object.keys(original)])

    for (const key of allKeys) {
      if (!isEqual(current[key], original[key])) {
        return false
      }
    }
    return true
  }

  /**
   * 检测是否有未保存的更改
   */
  const hasUnsavedChanges = () => {
    if (!config.value || !snapshots.value.config) {
      return false
    }

    // 序列化当前配置用于比较
    const tempConfig = deepClone(config.value)
    tempConfig.resolutions = serializeResolutions(resolutions.value)
    tempConfig.fps = serializeFps(filterValidFps(fps.value))
    tempConfig.global_prep_cmd = JSON.stringify(global_prep_cmd.value)
    tempConfig.display_mode_remapping = JSON.stringify(display_mode_remapping.value)

    return !configsAreEqual(tempConfig, snapshots.value.config)
  }

  return {
    platform,
    saved,
    restarted,
    config,
    fps,
    resolutions,
    currentTab,
    global_prep_cmd,
    display_mode_remapping,
    tabs,
    initTabs,
    loadConfig,
    save,
    apply,
    getRiskyChanges,
    handleHash,
    hasUnsavedChanges,
  }
}
