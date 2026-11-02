import test from 'node:test'
import assert from 'node:assert/strict'
import {
  formatRelativeSeen,
  getStreamingDestination,
  getStreamingState,
  parseHomeClients,
  pruneRecentConnectionStores,
  readRecentConnections,
  updateRecentConnections,
  writeRecentConnections,
} from '../utils/recentConnections.js'
import { getHomeCopy } from '../i18n/home.js'
import { openNativeDisplaySettings } from '../utils/nativeHomeNavigation.js'

test('arrival text names the destination; unnamed and multi-screen handled', () => {
  const copy = getHomeCopy('zh')
  assert.equal(getStreamingDestination([{ streaming: false }], copy), null)
  assert.equal(getStreamingDestination([{ uuid: 'tv', name: 'TV', streaming: true }], copy).target, 'TV')
  assert.equal(getStreamingDestination([{ uuid: 'tv', streaming: true }], copy).target, copy.arrivalUnnamed)
  const both = [{ uuid: 'a', streaming: true }, { uuid: 'b', streaming: true }]
  assert.equal(getStreamingDestination(both, copy).target, '2 块屏幕')
  assert.equal(getStreamingDestination(both, getHomeCopy('en')).target, '2 screens')
  assert.equal(getStreamingDestination(both, copy).key, getStreamingDestination([...both].reverse(), copy).key)
})

test('streaming status separates live, idle, loading and unavailable', () => {
  assert.equal(getStreamingState(null), 'loading')
  assert.equal(getStreamingState([]), 'idle')
  assert.equal(getStreamingState([{ streaming: false }]), 'idle')
  assert.equal(getStreamingState([{ streaming: true }]), 'streaming')
  assert.equal(getStreamingState([{ uuid: 'x' }]), 'unavailable')
  assert.equal(getStreamingState([{ streaming: true }], true), 'unavailable')
})

test('malformed list responses throw instead of faking an empty state', () => {
  for (const payload of [{}, { status: false, named_certs: [] }, { status: 'true', named_certs: null }])
    assert.throws(() => parseHomeClients(payload))
  assert.deepEqual(parseHomeClients({ status: 'true', named_certs: [] }), [])
})

test('merge keeps observed history, adopts newer host facts, drops the rest', () => {
  const now = 86400000 * 30
  const tvIdle = (extra = {}) => ({ uuid: 'tv', name: 'TV', streaming: false, ...extra })

  // a host-side fact alone fills history; a device never seen stays out
  assert.deepEqual(updateRecentConnections([], [tvIdle({ lastSessionEnded: now - 1 })], now), [
    { uuid: 'tv', name: 'TV', lastSeen: now - 1 },
  ])
  // newer local observation beats an older host fact, and vice versa
  const observed = [{ uuid: 'tv', name: 'TV', lastSeen: now - 3600000 }]
  assert.equal(
    updateRecentConnections(observed, [tvIdle({ lastSessionEnded: now - 5 * 86400000 })], now)[0].lastSeen,
    now - 3600000,
  )
  assert.equal(updateRecentConnections([], [tvIdle({ lastSessionEnded: now - 60000 })], now)[0].lastSeen, now - 60000)
  // live stream stamps now; future facts clamp to now; unseen devices stay out
  assert.equal(updateRecentConnections([], [{ uuid: 'tv', name: 'TV', streaming: true }], now)[0].lastSeen, now)
  assert.equal(updateRecentConnections([], [tvIdle({ lastSessionEnded: now + 99999 })], now)[0].lastSeen, now)
  assert.deepEqual(updateRecentConnections([], [tvIdle()], now), [])
})

test('history renames from authoritative clients, unpairs cleanly, stays bounded', () => {
  const records = updateRecentConnections([], [{ uuid: 'a', name: 'TV', streaming: true }], 1000)
  assert.deepEqual(records, [{ uuid: 'a', name: 'TV', lastSeen: 1000 }])
  assert.deepEqual(
    updateRecentConnections(records, [{ uuid: 'a', name: 'Renamed', streaming: false }], 2000),
    [{ uuid: 'a', name: 'Renamed', lastSeen: 1000 }],
  )
  assert.deepEqual(updateRecentConnections(records, [], 2000), [])
  const stale = [null, { uuid: 'a', lastSeen: 'x' }, { uuid: 'ghost', lastSeen: 5000 }, { uuid: 'a', lastSeen: 5000 }]
  // future-dated observations are rejected, so nothing survives at now=1000
  assert.deepEqual(updateRecentConnections(stale, [{ uuid: 'a' }], 1000), [])
  const many = Array.from({ length: 10 }, (_, i) => ({ uuid: String(i), name: String(i) }))
  const bounded = updateRecentConnections(many.map((c, i) => ({ ...c, lastSeen: i + 1 })), many, 100)
  assert.equal(bounded.length, 8)
  assert.equal(bounded[0].uuid, '9')
})

test('relative seen labels tier by recency; old or invalid fall back to absolute', () => {
  const zh = getHomeCopy('zh').time
  const en = getHomeCopy('en').time
  const now = 86400000 * 30
  assert.equal(formatRelativeSeen(now - 30000, zh, now), '刚刚')
  assert.equal(formatRelativeSeen(now - 5 * 60000, zh, now), '5 分钟前')
  assert.equal(formatRelativeSeen(now - 3 * 3600000, zh, now), '3 小时前')
  assert.equal(formatRelativeSeen(now - 30 * 3600000, zh, now), '昨天')
  assert.equal(formatRelativeSeen(now - 3 * 86400000, zh, now), '3 天前')
  assert.equal(formatRelativeSeen(now - 8 * 86400000, zh, now), '')
  assert.equal(formatRelativeSeen(now - 90 * 60000, en, now), '1 h ago')
  assert.equal(formatRelativeSeen('bad', zh, now), '')
  assert.equal(formatRelativeSeen(now + 5000, zh, now), '')
  assert.equal(formatRelativeSeen(now - 5000, undefined, now), '')
})

test('storage failures never break the page; stale sibling stores get swept', () => {
  assert.deepEqual(readRecentConnections({ getItem: () => '{broken' }, 'k'), [])
  assert.deepEqual(readRecentConnections({ getItem: () => { throw new Error('Blocked') } }, 'k'), [])
  assert.doesNotThrow(() =>
    writeRecentConnections({ setItem: () => { throw new Error('Quota') } }, 'k', []),
  )

  const now = Date.now()
  const stores = new Map([
    ['sunshine-recent-connections:old:1', JSON.stringify([{ uuid: 'a', lastSeen: now - 86400000 * 120 }])],
    ['sunshine-recent-connections:live:1', JSON.stringify([{ uuid: 'a', lastSeen: now - 86400000 }])],
    ['sunshine-recent-connections:junk:1', '{broken'],
    ['unrelated', 'keep'],
  ])
  const storage = {
    length: stores.size,
    key: (index) => [...stores.keys()][index],
    getItem: (key) => stores.get(key) ?? null,
    removeItem: (key) => stores.delete(key),
  }
  pruneRecentConnectionStores(storage, 'sunshine-recent-connections:live:1', now)
  assert.ok(!stores.has('sunshine-recent-connections:old:1'), 'stale store swept')
  assert.ok(!stores.has('sunshine-recent-connections:junk:1'), 'corrupt store swept')
  assert.ok(stores.has('sunshine-recent-connections:live:1') && stores.has('unrelated'))
  assert.doesNotThrow(() =>
    pruneRecentConnectionStores(
      { get length() { throw new Error('x') }, key: () => null, getItem: () => null, removeItem: () => {} },
      'k',
    ),
  )
})

test('home copy keeps first-use and empty-history apart across locales', () => {
  for (const locale of ['zh', 'zh_TW', 'en', 'en_US', 'ja']) {
    const copy = getHomeCopy(locale)
    assert.notEqual(copy.noDevices, copy.noHistory)
    assert.ok(copy.support.entry && copy.whisper.lateNight)
  }
  assert.equal(getHomeCopy('zh-TW').recent, '最近連線')
})

test('virtual display navigation targets only an embedded trusted shell', () => {
  const messages = []
  const parent = { postMessage: (...args) => messages.push(args) }
  const shell = {
    isTauri: true,
    parent,
    location: { ancestorOrigins: ['https://tauri.localhost'] },
  }
  assert.equal(openNativeDisplaySettings(shell, { referrer: '' }), true)
  assert.deepEqual(messages, [
    [{ type: 'native-vdd-open-request', source: 'sunshine-webui' }, 'https://tauri.localhost'],
  ])
  assert.equal(
    openNativeDisplaySettings({ ...shell, location: { ancestorOrigins: ['https://evil.example'] } }, { referrer: '' }),
    false,
  )
  assert.equal(openNativeDisplaySettings({ ...shell, isTauri: false }, {}), false)
  const standalone = { isTauri: true }
  standalone.parent = standalone
  assert.equal(openNativeDisplaySettings(standalone, {}), false)
})
