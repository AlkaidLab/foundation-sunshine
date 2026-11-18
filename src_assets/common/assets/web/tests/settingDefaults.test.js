import assert from 'node:assert/strict'
import { readFileSync, readdirSync } from 'node:fs'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'
import test from 'node:test'
import { SETTING_META } from '../../../../../../ds5-ui-m3e/src/settings/meta.js'

const WEB = dirname(fileURLToPath(import.meta.url))
const ROOT = fileURLToPath(new URL('../../../../../', import.meta.url))
const CONFIG_CPP = readFileSync(join(ROOT, 'src/config.cpp'), 'utf8')
const NVENC_HEADER = readFileSync(join(ROOT, 'src/nvenc/nvenc_config.h'), 'utf8')
const AMF_HEADER = readFileSync(join(ROOT, 'src/amf/amf_config.h'), 'utf8')
const LOCALE_DIR = join(ROOT, 'src_assets/common/assets/web/public/assets/locale')
const NETWORK_VUE = readFileSync(join(ROOT, 'src_assets/common/assets/web/configs/tabs/Network.vue'), 'utf8')
const DISPLAY_ADVANCED_VUE = readFileSync(join(ROOT, 'src_assets/common/assets/web/configs/tabs/display/DisplayAdvanced.vue'), 'utf8')
const INPUTS_VUE = readFileSync(join(ROOT, 'src_assets/common/assets/web/configs/tabs/Inputs.vue'), 'utf8')
const CFG_EXPERT_VUE = readFileSync(join(ROOT, 'src_assets/common/assets/web/components/cfg/CfgExpert.vue'), 'utf8')
const NVIDIA_VUE = readFileSync(join(ROOT, 'src_assets/common/assets/web/configs/tabs/encoders/NvidiaNvencEncoder.vue'), 'utf8')

// These are the settings whose locale text was audited in this change. The
// list covers every locale-specific edit, including labels and descriptions
// that only exist in one translation.
const LOCALE_AUDIT_KEYS = new Set([
  'amd_advanced_group_desc', 'amd_av1_latency_mode_desc', 'amd_avcodec_compat_desc',
  'amd_hevc_gop_size_desc', 'amd_high_motion_qb_desc', 'amd_input_queue_size_desc',
  'amd_lowlatency_mode_desc', 'amd_multi_hw_instance_desc', 'amd_quality_balanced',
  'amd_qvbr_quality_desc', 'amd_rc_cbr', 'amd_usage_desc', 'amd_vpe_desc',
  'back_button_timeout_desc', 'close_verify_safe_desc', 'coder_auto',
  'dsu_server_port_desc', 'dynamic_resolution_follow_display_desc', 'fec_percentage_desc',
  'ffmpeg_auto', 'hdr_luminance_analysis_desc', 'nvenc_frame_budget_guard_desc',
  'nvenc_preset_4', 'nvenc_spatial_aq_desc', 'nvenc_spatial_aq_enabled',
  'nvenc_split_encode_driver_decides_def', 'nvenc_twopass_quarter_res',
  'nvenc_vbv_increase_desc', 'nvenc_vdd_force_vsync_disabled', 'pair_max_attempts_desc',
  'qsv_preset_medium', 'res_fps_desc', 'sw_preset_desc', 'sw_preset_superfast',
  'sw_tune_desc', 'sw_tune_zerolatency', 'wan_encryption_mode_1', 'webhook_events_desc',
])

// A default-looking phrase is allowed only where it explains behavior (for
// example, an empty value delegates to the driver) or documents a different
// default source. Bare duplicate claims are removed from the locale strings.
const BEHAVIOR_DEFAULT_KEYS = new Set([
  'amd_advanced_group_desc', 'amd_av1_latency_mode_desc', 'amd_avcodec_compat_desc',
  'amd_hevc_gop_size_desc', 'amd_high_motion_qb_desc', 'amd_input_queue_size_desc',
  'amd_lowlatency_mode_desc', 'amd_multi_hw_instance_desc', 'amd_vpe_desc',
  'back_button_timeout_desc', 'close_verify_safe_desc', 'dynamic_resolution_follow_display_desc',
  'fec_percentage_desc', 'hdr_luminance_analysis_desc', 'nvenc_frame_budget_guard_desc',
  'nvenc_vbv_increase_desc', 'res_fps_desc', 'sw_tune_desc',
])

const NO_POPUP_DEFAULT_LABEL_KEYS = new Set([
  'ffmpeg_auto', 'nvenc_preset_4', 'qsv_preset_medium', 'sw_preset_superfast', 'wan_encryption_mode_1',
])

const STATIC_DEFAULT_KEYS = new Set([
  'amd_quality_balanced', 'amd_qvbr_quality_desc', 'amd_rc_cbr', 'coder_auto',
  'ffmpeg_auto', 'nvenc_preset_4', 'nvenc_spatial_aq_desc', 'nvenc_spatial_aq_enabled',
  'nvenc_split_encode_driver_decides_def', 'nvenc_twopass_quarter_res',
  'nvenc_vdd_force_vsync_disabled', 'pair_max_attempts_desc', 'qsv_preset_medium',
  'sw_preset_desc', 'sw_preset_superfast', 'sw_tune_desc', 'sw_tune_zerolatency',
  'wan_encryption_mode_1', 'webhook_events_desc',
])

// Values are the host's effective value when the key is absent. Optional AMF values use null: the driver
// receives no property. The visible Web UI uses an empty option for that state.
const HOST_DEFAULTS = Object.freeze({
  locale: 'en', sunshine_name: null, min_log_level: 2, sleep_mode: '0', notify_pre_releases: false, system_tray: true,
  stop_on_last_video_session: false, launch_splash: true, controller: true, gamepad: 'auto', usb_forwarding_enabled: false,
  usb_forwarding_port: 0, ds4_back_as_touchpad_click: true, motion_as_ds4: true, touchpad_as_ds4: true,
  enable_dsu_server: false, dsu_server_port: 26760, back_button_timeout: -1, keyboard: true, key_repeat_delay: 500,
  key_repeat_frequency: 24.9, always_send_scancodes: true, key_rightalt_to_key_win: false, mouse: true,
  high_resolution_scrolling: true, native_pen_touch: true, native_touchpad_optimization: true, capture_cursor: true,
  amf_draw_mouse_cursor: false, virtual_mouse: true, audio_sink: null, virtual_sink: null, keep_sink_default: false,
  install_steam_audio_drivers: true, stream_audio: true, mat_live: false, stream_mic: true, max_bitrate: 0,
  output_name: null, vdd_keep_enabled: false, vdd_headless_create: false, vdd_reuse: false, variable_refresh_rate: false,
  minimum_fps_target: 0, microphone_redirect_backend: 'auto', display_device_prep: 'no_operation', resolution_change: 'automatic',
  manual_resolution: null, refresh_rate_change: 'automatic', manual_refresh_rate: null, hdr_prep: 'automatic',
  dynamic_resolution_follow_display: true, hdr_luminance_analysis: 'auto', nvenc_vdd_force_vsync: false,
  vdd_vulkan_hdr_bridge: true, mdns_broadcast: true, upnp: false, address_family: 'ipv4', bind_address: null,
  external_endpoints: null, port: 47989, fec_auto: false, fec_percentage: 20, fec_auto_max_percentage: 50,
  origin_web_ui_allowed: 'lan', lan_encryption_mode: '0', wan_encryption_mode: '1', pair_max_attempts: 10,
  ping_timeout: 10000, close_verify_safe: false, file_apps: 'apps.json', credentials_file: 'sunshine_state.json',
  file_state: 'sunshine_state.json', log_path: 'sunshine.log', pkey: null, cert: null, adapter_name: null, capture: null,
  encoder: null, hevc_mode: 0, av1_mode: 0, capture_target: 'display', window_title: null, wgc_disable_secure_desktop: false,
  capture_compute_shader: 'auto', nvenc_preset: 4, nvenc_frame_budget_guard: true, nvenc_twopass: 'quarter_res',
  nvenc_spatial_aq: true, nvenc_rate_control: 'cbr', nvenc_target_quality: 0, nvenc_vbv_increase: 0,
  nvenc_realtime_hags: true, nvenc_split_encode: 'driver_decides', nvenc_manage_driver_settings: true,
  nvenc_latency_over_power: true, nvenc_opengl_vulkan_on_dxgi: true, nvenc_cuda_array_input: false, nvenc_h264_cavlc: false,
  qsv_preset: 'medium', qsv_coder: 'auto', qsv_slow_hevc: false, amd_usage: null, amd_slices_per_frame: 0,
  amd_avcodec_compat: false, amd_rc: null, amd_enforce_hrd: null, amd_vpe: false, amd_qvbr_quality: 23, amd_quality: null,
  amd_preanalysis: null, amd_vbaq: null, amd_coder: 'auto', amd_hevc_gop_size: -1, amd_high_motion_qb: null,
  amd_lowlatency_mode: null, amd_multi_hw_instance: null, amd_input_queue_size: null, amd_av1_latency_mode: null,
  vt_coder: 'auto', vt_software: 'auto', vt_realtime: true, sw_preset: 'superfast', sw_tune: 'zerolatency', min_threads: 2,
})

const normalize = (value) => {
  if (value === undefined || value === null || value === '') return null
  if (value === true || ['true', 'enabled', 'on'].includes(String(value))) return true
  if (value === false || ['false', 'disabled', 'off'].includes(String(value))) return false
  const number = Number(value)
  return Number.isFinite(number) && String(value).trim() !== '' ? number : String(value)
}

const attr = (tag, name) => tag.match(new RegExp(`\\b${name}="([^"]*)"`))?.[1]
const vueFiles = []
const visit = (directory) => {
  for (const entry of readdirSync(directory, { withFileTypes: true })) {
    const path = join(directory, entry.name)
    if (entry.isDirectory()) visit(path)
    else if (entry.name.endsWith('.vue')) vueFiles.push(path)
  }
}
visit(join(ROOT, 'src_assets/common/assets/web/configs'))

const uiDefaults = new Map()
for (const path of vueFiles) {
  const source = readFileSync(path, 'utf8')
  for (const match of source.matchAll(/<Cfg[A-Za-z]+[\s\S]*?>/g)) {
    const tag = match[0]
    const key = attr(tag, 'k')
    if (!key) continue
    const explicit = attr(tag, 'def')
    const value = explicit !== undefined ? explicit : SETTING_META[key]?.def
    if (!uiDefaults.has(key)) uiDefaults.set(key, { value, path })
  }
}

// These source assertions make the table fail when a host default is moved without a matching UI audit.
test('host-default table is anchored to the current C++ defaults', () => {
  assert.match(NVENC_HEADER, /int quality_preset = 4;/)
  assert.match(NVENC_HEADER, /bool adaptive_quantization = true;/)
  assert.match(CONFIG_CPP, /true,\s*\/\/ dynamic_resolution_follow_display/)
  assert.match(CONFIG_CPP, /qsv::medium,\s*\/\/ preset/)
  assert.match(CONFIG_CPP, /"superfast"s,\s*\/\/ preset/)
  assert.match(CONFIG_CPP, /std::nullopt,\s*\/\/ enforce_hrd/)
  assert.match(AMF_HEADER, /std::optional<bool> high_motion_quality_boost_enable;/)
})

test('every visible setting default matches the host default table', () => {
  const missing = []
  const mismatches = []
  for (const [key, entry] of uiDefaults) {
    if (!(key in HOST_DEFAULTS)) { missing.push(key); continue }
    const actual = normalize(entry.value)
    const expected = normalize(HOST_DEFAULTS[key])
    if (actual !== expected) mismatches.push(`${key}: UI=${String(entry.value)} host=${String(HOST_DEFAULTS[key])} (${entry.path})`)
  }
  assert.deepEqual(missing, [], `visible settings missing from host-default table: ${missing.join(', ')}`)
  assert.deepEqual(mismatches, [], mismatches.join('\n'))
})

test('localized setting descriptions do not repeat a static default beside the popup default', () => {
  const claim = /default|standard|par défaut|por defeito|por defecto|predeterm|predefinit|по умолч|по подразбиране|domyśl|výchoz|alapér|стандарт|默认|デフォルト|既定|기본값|padrão|predvolen|defeito/i
  const ignored = new Set(['keep_sink_default_desc', 'audio_sink_desc_linux', 'audio_sink_desc_windows', 'output_name_desc_windows', 'log_level_desc', 'qsv_preset_desc', ...BEHAVIOR_DEFAULT_KEYS, ...NO_POPUP_DEFAULT_LABEL_KEYS])
  const violations = []
  for (const file of readdirSync(LOCALE_DIR).filter((name) => name.endsWith('.json'))) {
    const locale = JSON.parse(readFileSync(join(LOCALE_DIR, file), 'utf8'))
    for (const [key, value] of Object.entries(locale.config || {})) {
      if ((key.endsWith('_desc') || STATIC_DEFAULT_KEYS.has(key)) && !ignored.has(key) && claim.test(String(value))) violations.push(`${file}:${key}`)
    }
  }
  assert.deepEqual(violations, [], violations.join(', '))
})

test('non-popup default option labels remain marked in every locale', () => {
  const marker = /default|standard|défaut|defecto|predeterm|predefin|padr[aã]o|domyś|výchoz|подразбира|по умолч|замовч|типов|varsayılan|デフォルト|既定|기본값|默认|預設/i
  const missing = []
  for (const file of readdirSync(LOCALE_DIR).filter((name) => name.endsWith('.json'))) {
    const locale = JSON.parse(readFileSync(join(LOCALE_DIR, file), 'utf8'))
    for (const key of NO_POPUP_DEFAULT_LABEL_KEYS) {
      if (!marker.test(String(locale.config?.[key] || ''))) missing.push(`${file}:${key}`)
    }
  }
  assert.deepEqual(missing, [], `unmarked non-popup default labels: ${missing.join(', ')}`)
})

test('numeric controls expose the host accepted ranges', () => {
  assert.match(NETWORK_VUE, /<CfgExpert[^>]*k="fec_percentage"[^>]*min="0"[^>]*max="255"/)
  assert.match(DISPLAY_ADVANCED_VUE, /<CfgExpert[^>]*k="minimum_fps_target"[^>]*min="0"[^>]*max="1000"/)
  assert.match(CONFIG_CPP, /int_between_f\(vars, "fec_percentage",[^\n]*\{ 0, 255 \}/)
  assert.match(CONFIG_CPP, /int_between_f\(vars, "minimum_fps_target",[^\n]*\{ 0, 1000 \}/)
})

test('sentinel defaults use the same wording in the control and popup', () => {
  assert.match(INPUTS_VUE, /<CfgExpert[^>]*k="back_button_timeout"[^>]*off-text="disabled"/)
  assert.match(CFG_EXPERT_VUE, /offText: \{ type: String, default: undefined \}/)
  assert.match(CFG_EXPERT_VUE, /const offText = computed\(\(\) => props\.offText \?\? meta\.value\.offText\)/)
  assert.match(CFG_EXPERT_VUE, /t\(offText\.value === 'auto' \? '_common\.auto' : '_common\.disabled'\)/)
})

test('preset default control uses the host NVENC P4 default', () => {
  assert.match(NVIDIA_VUE, /<CfgSteps[^>]*k="nvenc_preset"[^>]*def="4"/)
  assert.match(NVENC_HEADER, /int quality_preset = 4;/)
})

test('audited locale text keeps CJK punctuation tight and Latin spacing singular', () => {
  const violations = []
  for (const file of readdirSync(LOCALE_DIR).filter((name) => name.endsWith('.json'))) {
    const locale = JSON.parse(readFileSync(join(LOCALE_DIR, file), 'utf8'))
    for (const [key, value] of Object.entries(locale.config || {})) {
      if (!LOCALE_AUDIT_KEYS.has(key) || typeof value !== 'string') continue
      if (/[。；，] /.test(value) || / {2,}/.test(value)) violations.push(`${file}:${key}: ${value}`)
    }
  }
  assert.deepEqual(violations, [], violations.join('\n'))
})
