// SPDX-License-Identifier: GPL-3.0-only
// The checks of displaypages.test.js of foundation-ui that read this product's settings data (the search index and the label tables): they live here because the data does.
import assert from 'node:assert/strict'
import test from 'node:test'
import './helpers/foundationUi.mjs'
const { SEARCH_INDEX, LABELS } = await import('../config/settingsData/index.js')
const { parseSettingsHash, settingsHash } = await import('foundation-ui/settings/index.js')
const { VDD_PARTS } = await import('foundation-ui/settings/virtualDisplay.js')

const where = (id) => SEARCH_INDEX.find((e) => e.id === id)?.params

test('DS1: the settings of the old 音频/视频 are on 显示与视频 or on 音频, each once, where the search finds them', () => {
  const at = {
    hdr_prep: ['display', 'hdr'], hdr_luminance_analysis: ['display', 'hdr'], vdd_vulkan_hdr_bridge: ['display', 'hdr'], variable_refresh_rate: ['display', 'output'], fps: ['display', 'modes'], resolutions: ['display', 'modes'],
    adapter_name: ['display', 'advanced'], output_name: ['display', 'output'], display_device_prep: ['display', 'output'], resolution_change: ['display', 'modes'], refresh_rate_change: ['display', 'modes'],
    manual_resolution: ['display', 'modes'], manual_refresh_rate: ['display', 'modes'], dynamic_resolution_follow_display: ['display', 'modes'], vdd_reuse: ['display', 'virtual'], vdd_headless_create: ['display', 'output'],
    vdd_keep_enabled: ['display', 'virtual'], nvenc_vdd_force_vsync: ['display', 'advanced'], max_bitrate: ['display', 'encoders'], minimum_fps_target: ['display', 'advanced'],
    stream_audio: ['audio', 'sound'], audio_sink: ['audio', 'sound'], virtual_sink: ['audio', 'sound'], keep_sink_default: ['audio', 'sound'], install_steam_audio_drivers: ['audio', 'sound'],
    stream_mic: ['audio', 'microphone'], microphone_redirect_backend: ['audio', 'microphone'], mat_live: ['display', 'advanced'],
  }
  for (const [key, [tab, place]] of Object.entries(at)) {
    const w = where(key)
    assert.ok(w, `${key} is in the index`)
    assert.deepEqual([w.tab, w.sub], [tab, place], key)
    assert.equal(SEARCH_INDEX.filter((e) => e.id === key).length, 1, `${key} once`)
  }
  assert.ok(!SEARCH_INDEX.some((e) => e.params.tab === 'av'), 'nothing is on the old tab')
})

test('DS1: an old address lands on the moved setting: by section, by key, by tab and key, the plain key, the bare old tab', () => {
  assert.deepEqual(parseSettingsHash('#tab=av&sub=display', SEARCH_INDEX), { tab: 'display', sub: 'hdr' })
  assert.deepEqual(parseSettingsHash('#tab=av&sub=display&focus=hdr_prep', SEARCH_INDEX), { tab: 'display', sub: 'hdr', focus: 'hdr_prep' })
  assert.deepEqual(parseSettingsHash('#tab=av&sub=virtual', SEARCH_INDEX), { tab: 'display', sub: 'virtual' })
  assert.deepEqual(parseSettingsHash('#tab=av&sub=virtual&focus=vdd_reuse', SEARCH_INDEX), { tab: 'display', sub: 'virtual', focus: 'vdd_reuse' })
  assert.deepEqual(parseSettingsHash('#tab=av&sub=microphone', SEARCH_INDEX), { tab: 'audio', sub: 'microphone' })
  assert.deepEqual(parseSettingsHash('#tab=av&sub=expert&focus=virtual_sink', SEARCH_INDEX), { tab: 'audio', sub: 'sound', focus: 'virtual_sink' })
  assert.deepEqual(parseSettingsHash('#tab=av&sub=expert&focus=minimum_fps_target', SEARCH_INDEX), { tab: 'display', sub: 'advanced', focus: 'minimum_fps_target' })
  assert.deepEqual(parseSettingsHash('#tab=av&focus=hdr_prep', SEARCH_INDEX), { tab: 'display', sub: 'hdr', focus: 'hdr_prep' })
  assert.deepEqual(parseSettingsHash('#tab=av&focus=stream_audio', SEARCH_INDEX), { tab: 'audio', sub: 'sound', focus: 'stream_audio' })
  assert.deepEqual(parseSettingsHash('#tab=av&focus=max_bitrate', SEARCH_INDEX), { tab: 'display', sub: 'encoders', focus: 'max_bitrate' })
  assert.deepEqual(parseSettingsHash('#tab=advanced&focus=adapter_name', SEARCH_INDEX), { tab: 'display', sub: 'advanced', focus: 'adapter_name' })
  assert.deepEqual(parseSettingsHash('#adapter_name', SEARCH_INDEX), { tab: 'display', sub: 'advanced', focus: 'adapter_name' })
  assert.deepEqual(parseSettingsHash('#tab=av', SEARCH_INDEX), { tab: 'display', sub: 'advanced' }, 'the bare old tab opens on 高级, the renamed 捕获与视频')
  assert.deepEqual(parseSettingsHash('#av', SEARCH_INDEX), { tab: 'display', sub: 'advanced' })
  assert.deepEqual(parseSettingsHash('#display', SEARCH_INDEX), { tab: 'display', sub: 'output' }, 'no place named: the first')
  assert.deepEqual(parseSettingsHash('#audio', SEARCH_INDEX), { tab: 'audio', sub: 'sound' })
  assert.equal(settingsHash({ tab: 'audio', sub: 'microphone' }), '#tab=audio&sub=microphone')
})

test('DS1: every key formerly on 高级 or 编码器 is on exactly one display place, and old tabs land there', () => {
  const oldKeys = ['codec_strategy', 'codec_enable_hdr', 'hdr_enhanced', 'adapter_name', 'capture', 'hevc_mode', 'av1_mode', 'capture_target', 'window_title', 'wgc_disable_secure_desktop', 'capture_compute_shader', 'encoder',
    'nvenc_preset', 'nvenc_frame_budget_guard', 'nvenc_twopass', 'nvenc_spatial_aq', 'nvenc_rate_control', 'nvenc_target_quality', 'nvenc_vbv_increase', 'nvenc_realtime_hags', 'nvenc_split_encode', 'nvenc_manage_driver_settings', 'nvenc_latency_over_power', 'nvenc_opengl_vulkan_on_dxgi', 'nvenc_cuda_array_input', 'nvenc_h264_cavlc',
    'qsv_preset', 'qsv_coder', 'qsv_slow_hevc', 'amd_usage', 'amd_slices_per_frame', 'amd_avcodec_compat', 'amd_vpe', 'amd_rc', 'amd_enforce_hrd', 'amd_qvbr_quality', 'amd_quality', 'amd_preanalysis', 'amd_vbaq', 'amd_coder', 'amd_hevc_gop_size', 'amd_high_motion_qb', 'amd_lowlatency_mode', 'amd_multi_hw_instance', 'amd_input_queue_size', 'amd_av1_latency_mode', 'vt_coder', 'vt_software', 'vt_realtime', 'sw_preset', 'sw_tune', 'min_threads']
  const entries = oldKeys.map((key) => SEARCH_INDEX.filter((e) => e.id === key))
  assert.ok(entries.every((matches) => matches.length === 1))
  assert.ok(entries.every(([entry]) => entry.params.tab === 'display'))
  assert.ok(entries.every(([entry]) => ['encoders', 'advanced', 'hdr', 'output'].includes(entry.params.sub)))
  assert.equal(new Set(oldKeys).size, oldKeys.length)
  assert.deepEqual(parseSettingsHash('#tab=advanced', SEARCH_INDEX), { tab: 'display', sub: 'advanced' })
  assert.deepEqual(parseSettingsHash('#tab=encoders', SEARCH_INDEX), { tab: 'display', sub: 'encoders' })
  assert.deepEqual(parseSettingsHash('#tab=encoders&sub=nv&focus=nvenc_preset', SEARCH_INDEX), { tab: 'display', sub: 'encoders', focus: 'nvenc_preset' })
  assert.deepEqual(parseSettingsHash('#nv', SEARCH_INDEX), { tab: 'display', sub: 'encoders' })
})

test('DS1: the parts of the virtual display control are the ones the pages draw, each setting in one part', () => {
  const all = Object.values(VDD_PARTS).flat()
  assert.equal(new Set(all).size, all.length)
  for (const key of all) assert.ok(where(key), `${key} is searchable`)
})
