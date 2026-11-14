import test from 'node:test'
import assert from 'node:assert/strict'
import { countLogLevels, deviceIconFor, formatBitrate, formatUptime, pickPerf, tileAvailable } from '../utils/homeData.js'
import { DEFAULT_TILES, HOME_TILES } from '../config/homeTiles.js'
import { SPONSORS } from '../config/sponsors.js'

test('device icons come from the keyword table and fall back to the neutral icon', () => {
  const cases = { 'linux-test-host MacBook': 'laptop_mac', 'iPhone 15 Pro': 'mobile', 'Steam Deck': 'sports_esports', 'Living room TV': 'tv', 'Pixel 9': 'mobile', 'vivo X100': 'mobile', 'Xiaomi Pad 6': 'tablet', 'iMac': 'desktop_mac', 'iPad Air': 'tablet', 'DESKTOP-4K2L': 'desktop_windows', 'Android TV': 'tv', 'Quest 3': 'view_in_ar', 'Unnamed device': 'devices', '': 'devices' }
  for (const [name, icon] of Object.entries(cases)) assert.equal(deviceIconFor(name), icon, name)
  assert.equal(deviceIconFor(null), 'devices')
  // the reported system fills in what a name does not say, and a name cannot turn a system into another one
  assert.equal(deviceIconFor('K60', 'android'), 'mobile')
  assert.equal(deviceIconFor('Zako', 'macos'), 'laptop_mac')
  assert.equal(deviceIconFor('Zako', 'windows'), 'desktop_windows')
  assert.equal(deviceIconFor('Zako', 'tvos'), 'tv')
  assert.equal(deviceIconFor('Quest 3', 'android'), 'view_in_ar')
  assert.equal(deviceIconFor('Zako', ''), 'devices')
})

test('bitrate and uptime formatting', () => {
  assert.deepEqual(formatBitrate(60000), { value: '60', unit: 'Mbps' })
  assert.deepEqual(formatBitrate(12500), { value: '12.5', unit: 'Mbps' })
  assert.deepEqual(formatBitrate(800), { value: '800', unit: 'kbps' })
  assert.equal(formatBitrate(0), null)
  assert.equal(formatBitrate(null), null)
  assert.equal(formatUptime(734000), '12:14')
  assert.equal(formatUptime(3723000), '1:02:03')
  assert.equal(formatUptime(undefined), '')
})

test('perf snapshot: the active session wins, missing numbers stay null, fps prefers the frame-rate collection', () => {
  const snap = { sessions: [
    { session_id: 1, active: false, bitrate_kbps: 1000 },
    { session_id: 2, active: true, fps: 120, bitrate_kbps: 60000, encoder: 'nvenc', uptime_ms: 5000, host_latency: { avg_ms: 3.9, p95_ms: 5.2, last_ms: 4.1, recent_fps: 118, series_ms: [3, 4, null, 5] }, frame_rates: { encoded: { fps: 119.4 } } },
  ] }
  const p = pickPerf(snap, 2)
  assert.equal(p.sessionId, 2)
  assert.equal(p.fps, 119.4)
  assert.deepEqual(p.series, [3, 4, 5])
  assert.equal(pickPerf({ sessions: [{ session_id: 3, active: true, host_latency: { recent_fps: 59.9, avg_ms: null } }] }).fps, 59.9)
  assert.equal(pickPerf({ sessions: [{ session_id: 3, active: true }] }).avgMs, null)
  assert.equal(pickPerf({ sessions: [{ active: false }] }), null)
  assert.equal(pickPerf(null), null)
})

test('log levels are counted and the latest error text has its level prefix removed', () => {
  const entries = [{ level: 'Info', value: 'Info: a' }, { level: 'Error', value: 'Error: first' }, { level: 'Warning', value: 'Warning: w' }, { level: 'Fatal', value: 'Fatal: boom' }]
  assert.deepEqual(countLogLevels(entries), { fatal: 1, error: 1, warning: 1, lastError: 'boom' })
  assert.deepEqual(countLogLevels(null), { fatal: 0, error: 0, warning: 0, lastError: '' })
})

test('tiles that need a source the host lacks are not available', () => {
  const byId = Object.fromEntries(HOME_TILES.map((t) => [t.id, t]))
  assert.equal(tileAvailable(byId.session, { local: false, platform: 'windows' }), false)
  assert.equal(tileAvailable(byId.session, { local: null, platform: 'linux' }), true)
  assert.equal(tileAvailable(byId.vdd, { local: true, platform: 'linux' }), false)
  assert.equal(tileAvailable(byId.vdd, { local: true, platform: 'windows' }), true)
  assert.equal(tileAvailable(byId.apps, { local: false, platform: 'linux' }), true)
})

test('catalogue: ids are unique, defaults exist, default size is allowed, defaults fill at most four rows of six cells', () => {
  const ids = HOME_TILES.map((t) => t.id)
  assert.equal(new Set(ids).size, ids.length)
  for (const id of DEFAULT_TILES) assert.ok(ids.includes(id), id)
  const cells = { '1x1': 1, '2x1': 2, '2x2': 4 }
  for (const t of HOME_TILES) assert.ok(t.sizes.includes(t.size), t.id)
  const size = (id) => cells[HOME_TILES.find((t) => t.id === id).size]
  // tiles that only exist during a stream are not on an idle page: idle fills at most four rows of six cells, a live page at most five
  const liveOnly = new Set(['latency', 'bitrate', 'fps', 'loss'])
  const idle = DEFAULT_TILES.filter((id) => !liveOnly.has(id)).reduce((n, id) => n + size(id), 0)
  const live = DEFAULT_TILES.reduce((n, id) => n + size(id), 0)
  assert.ok(idle <= 24, `idle default set takes ${idle} cells`)
  assert.ok(live <= 30, `live default set takes ${live} cells`)
  assert.ok(DEFAULT_TILES.includes('downloads') && DEFAULT_TILES.includes('sponsor'))
})

test('sponsor targets are https links with a label', () => {
  assert.ok(SPONSORS.length >= 2)
  for (const s of SPONSORS) { assert.match(s.url, /^https:\/\//); assert.ok(s.label) }
})

test('sponsor targets are the team\'s own 爱发电 pages in team order, the same list as the about page', async () => {
  const { SPONSORS: ABOUT } = await import('foundation-ui/about')
  assert.deepEqual(SPONSORS.map((s) => [s.label, s.url]), ABOUT.map((s) => [s.login, s.url]))
  assert.deepEqual(SPONSORS.map((s) => s.label), ['qiin2333', 'Yundi339', 'skyhua0224'])
})
