import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import './helpers/foundationUi.mjs'
// the settings model lives in the foundation-ui package (the sibling checkout), shared with the control panel
const { SEARCH_INDEX } = await import('foundation-ui/settings/searchIndex.js')
const { buildSettingsEntries, searchEntries } = await import('foundation-ui/settings/search.js')
const { labelOf } = await import('foundation-ui/settings/labels.js')

// Every key the host reads from its configuration must be reachable in the settings search (by label in Chinese and English
// and by its key name), unless it has no control in the Web UI at all. The list below is those keys: none of them had a
// control in the original Web UI either (checked against the commit before the redesign), they are set in the config file.
const NO_CONTROL = Object.freeze({
  amd_ltr_frames: 'no control in the original UI',
  client_fingerprint_remote_rules: 'no control in the original UI',
  client_fingerprint_rules_certificate: 'no control in the original UI',
  client_fingerprint_rules_refresh_hours: 'no control in the original UI',
  client_fingerprint_rules_url: 'no control in the original UI',
  client_gamepad_override: 'no control in the original UI',
  clients: 'the paired clients are managed on the clients page, not a setting',
  clipboard_sync: 'no control in the original UI',
  downscaling_quality: 'no control in the original UI',
  external_ip: 'replaced by external_endpoints, which the remote access card edits; the old key is still read',
  file_mapping_port: 'no control in the original UI',
  file_mappings: 'no control in the original UI',
  input_activity_boost: 'no control in the original UI',
  input_activity_boost_fps: 'no control in the original UI',
  input_activity_boost_window_ms: 'no control in the original UI',
  fs_host_icon: 'new with the specialised client interface (the icon token of the host for the macOS client; empty: the client chooses): no control in the redesigned UI, set in the config file',
  host_audio: 'runtime switch is on the current-stream page, not the settings search',
  keybindings: 'default kept by the configuration store, no control in the original UI',
  max_log_size_mb: 'no control in the original UI',
  nvenc_lookahead_depth: 'commented out in the original UI',
  nvenc_lookahead_level: 'commented out in the original UI',
  nvenc_temporal_filter: 'commented out in the original UI',
  qp: 'no control in the original UI',
  restore_log: 'no control in the original UI',
  session_absolute_hours: 'new with the web login sessions (12 h default, 1..720): the defaults suit a person, no control in the redesigned UI, set in the config file',
  session_idle_minutes: 'new with the web login sessions (30 min default, 1..1440): the defaults suit a person, no control in the redesigned UI, set in the config file',
  session_remember_days: 'new with the web login sessions (30 days default, 1..365): the defaults suit a person, no control in the redesigned UI, set in the config file',
  session_remember_idle_minutes: 'new with the web login sessions (7 days default, 1..525600): the defaults suit a person, no control in the redesigned UI, set in the config file',
  stun_servers: 'new with the network overview (empty: the built-in list of the region, "none": off): read by the overview, no control in the redesigned UI, set in the config file',
  tray_locale: 'no control in the original UI',
  vaapi_strict_rc_buffer: 'no control in the original UI',
  widget_token: 'no control in the original UI',
  video_pacing_threshold: 'new with the poster and pacing work (bytes, 0 = off, default 65536): no control in the redesigned UI, set in the config file',
})

const source = readFileSync(new URL('../../../../../src/config.cpp', import.meta.url), 'utf8')
const hostKeys = [...new Set([...source.matchAll(/_f\(\s*vars,\s*"([a-z0-9_]+)"/g)].map((m) => m[1]))].sort()

test('the host config definitions are read (a guard against the pattern going stale)', () => {
  assert.ok(hostKeys.length > 100)
  assert.ok(hostKeys.includes('hevc_mode') && hostKeys.includes('bind_address'))
})

test('every host configuration key is searchable, or listed as having no control', () => {
  const zh = buildSettingsEntries({ index: SEARCH_INDEX, label: (k) => labelOf('zh', k), altLabel: (k) => labelOf('en', k) })
  const en = buildSettingsEntries({ index: SEARCH_INDEX, label: (k) => labelOf('en', k), altLabel: (k) => labelOf('zh', k) })
  const missing = []
  for (const key of hostKeys) {
    const row = zh.find((e) => e.keyName === key)
    if (!row) { if (!(key in NO_CONTROL)) missing.push(key); continue }
    assert.ok(!(key in NO_CONTROL), `${key} has a control now: take it off the no-control list`)
    const rowEn = en.find((e) => e.keyName === key)
    assert.ok(searchEntries(zh, key, 8).includes(row), `${key} by key name`)
    assert.ok(searchEntries(zh, row.title, 12).some((e) => e.keyName === key), `${key} by Chinese label`)
    assert.ok(searchEntries(en, rowEn.title, 12).some((e) => e.keyName === key), `${key} by English label`)
  }
  assert.deepEqual(missing, [], `keys with no search entry and not on the no-control list: ${missing.join(', ')}`)
})

test('the no-control list names only keys the host has', () => {
  for (const key of Object.keys(NO_CONTROL)) assert.ok(hostKeys.includes(key), key)
})
