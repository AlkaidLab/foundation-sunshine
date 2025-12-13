import { ref } from 'vue'
import { trackEvents } from '../config/firebase.js'

// 平台相关的标签页排除规则
const PLATFORM_EXCLUSIONS = {
  windows: ['vt', 'vaapi'],
  linux: ['amd', 'qsv', 'vt'],
  macos: ['amd', 'nv', 'qsv', 'vaapi'],
}

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
      display_device_prep: 'no_operation',
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
          amd_usage: 'ultralowlatency',
          amd_rc: 'vbr_latency',
          amd_enforce_hrd: 'disabled',
          amd_quality: 'balanced',
          amd_preanalysis: 'disabled',
          amd_vbaq: 'enabled',
          amd_coder: 'auto',
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

// 不参与默认值比较的键
const EXCLUDED_DEFAULT_KEYS = new Set(['resolutions', 'fps', 'adapter_name'])

/**
 * 安全解析 JSON
 */
const safeParseJSON = (str, fallback = []) => {
  try {
    return JSON.parse(str || JSON.stringify(fallback))
  } catch {
    return fallback
  }
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

  // 初始化标签页配置
  const initTabs = () => {
    tabs.value = JSON.parse(JSON.stringify(DEFAULT_TABS))
  }

  // 加载配置
  const loadConfig = async () => {
    try {
      const response = await fetch('/api/config')
      const data = await response.json()

      platform.value = data.platform || ''

      // 根据平台过滤标签页
      const exclusions = PLATFORM_EXCLUSIONS[platform.value] || []
      tabs.value = tabs.value
        .map((tab) => {
          // 处理分组标签页（编码器）
          if (tab.type === 'group' && tab.children) {
            return {
              ...tab,
              children: tab.children.filter((child) => !exclusions.includes(child.id)),
            }
          }
          // 处理普通标签页
          if (!exclusions.includes(tab.id)) {
            return tab
          }
          return null
        })
        .filter(Boolean)

      // 移除不需要的字段
      const { platform: _, status, version, ...configData } = data
      config.value = configData

      // 填充默认值
      for (const tab of tabs.value) {
        // 处理分组标签页
        if (tab.type === 'group' && tab.children) {
          for (const childTab of tab.children) {
            for (const [key, defaultVal] of Object.entries(childTab.options)) {
              if (config.value[key] === undefined) {
                config.value[key] = defaultVal
              }
            }
          }
        } else if (tab.options) {
          // 处理普通标签页
          for (const [key, defaultVal] of Object.entries(tab.options)) {
            if (config.value[key] === undefined) {
              config.value[key] = defaultVal
            }
          }
        }
      }

      // 解析特殊字段
      fps.value = safeParseJSON(config.value.fps)

      try {
        const resStr = (config.value.resolutions || '').replace(/(\d+)x(\d+)/g, '"$1x$2"')
        resolutions.value = JSON.parse(resStr)
      } catch {
        resolutions.value = []
      }

      global_prep_cmd.value = safeParseJSON(config.value.global_prep_cmd)
      display_mode_remapping.value = safeParseJSON(config.value.display_mode_remapping)

      // 确保配置中有默认值
      config.value.global_prep_cmd = config.value.global_prep_cmd || []
      config.value.display_mode_remapping = config.value.display_mode_remapping || []
    } catch (error) {
      console.error('Failed to load config:', error)
    }
  }

  // 序列化配置
  const serialize = () => {
    // 序列化分辨率
    config.value.resolutions = JSON.stringify(resolutions.value)
      .replace(/","/g, ',')
      .replace(/^\["/, '[')
      .replace(/"\]$/, ']')

    // 过滤并序列化 FPS
    fps.value = fps.value.filter((item) => +item >= 30 && +item <= 500)
    config.value.fps = JSON.stringify(fps.value).replace(/"/g, '')

    config.value.global_prep_cmd = JSON.stringify(global_prep_cmd.value)
    config.value.display_mode_remapping = JSON.stringify(display_mode_remapping.value)
  }

  // 保存配置
  const save = async () => {
    saved.value = false
    restarted.value = false
    serialize()

    const configData = JSON.parse(JSON.stringify(config.value))

    // 删除默认值
    for (const tab of tabs.value) {
      // 处理分组标签页
      if (tab.type === 'group' && tab.children) {
        for (const childTab of tab.children) {
          for (const optionKey of Object.keys(childTab.options)) {
            if (shouldDeleteDefault(configData, childTab, optionKey)) {
              delete configData[optionKey]
            }
          }
        }
      } else if (tab.options) {
        // 处理普通标签页
        for (const optionKey of Object.keys(tab.options)) {
          if (shouldDeleteDefault(configData, tab, optionKey)) {
            delete configData[optionKey]
          }
        }
      }
    }

    try {
      const response = await fetch('/api/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(configData),
      })

      saved.value = response.ok

      if (saved.value) {
        trackEvents.configChanged(currentTab.value, 'save')
      }

      return saved.value
    } catch (error) {
      console.error('Save failed:', error)
      trackEvents.errorOccurred('config_save', error.message)
      return false
    }
  }

  // 应用配置（保存并重启）
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
      await fetch('/api/restart', { method: 'POST' })
      trackEvents.userAction('config_applied')
    } catch (error) {
      console.error('Failed to restart:', error)
    }
  }

  // 处理哈希导航
  const handleHash = () => {
    const hash = window.location.hash.slice(1)
    if (!hash) return

    // 查找普通标签页
    let targetTab = tabs.value.find(
      (tab) => tab.id === hash || (tab.options && Object.keys(tab.options).includes(hash))
    )

    // 如果在分组标签页中查找
    if (!targetTab) {
      for (const tab of tabs.value) {
        if (tab.type === 'group' && tab.children) {
          const childTab = tab.children.find((child) => child.id === hash || Object.keys(child.options).includes(hash))
          if (childTab) {
            targetTab = childTab
            break
          }
        }
      }
    }

    if (targetTab) {
      currentTab.value = targetTab.id
      setTimeout(() => {
        document.getElementById(hash)?.scrollIntoView({ behavior: 'smooth' })
      }, 100)
    }
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
    handleHash,
  }
}
