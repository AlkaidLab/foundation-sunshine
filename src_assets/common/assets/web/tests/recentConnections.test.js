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

test('arrival text names the actual destination and handles unnamed and simultaneous streams', () => {
  const copy = getHomeCopy('zh')
  assert.equal(getStreamingDestination([{ streaming: false }], copy), null)
  assert.equal(
    getStreamingDestination([{ uuid: 'tv', name: 'Living Room TV', streaming: true }], copy).target,
    'Living Room TV',
  )
  assert.equal(
    getStreamingDestination([{ uuid: 'tv', name: '  ', streaming: true }], copy).target,
    copy.arrivalUnnamed,
  )
  const active = [
    { uuid: 'tv', streaming: true },
    { uuid: 'pc', streaming: true },
  ]
  assert.equal(getStreamingDestination(active, copy).target, '2 块屏幕')
  assert.equal(getStreamingDestination(active, getHomeCopy('en')).target, '2 screens')
  assert.equal(
    getStreamingDestination(active, copy).key,
    getStreamingDestination([...active].reverse(), copy).key,
  )
})

test('streaming status separates live sessions, idle, loading, and unavailable data', () => {
  assert.equal(getStreamingState(null), 'loading')
  assert.equal(getStreamingState([]), 'idle')
  assert.equal(getStreamingState([{ streaming: false }]), 'idle')
  assert.equal(getStreamingState([{ streaming: false }, { streaming: true }]), 'streaming')
  assert.equal(getStreamingState([{ streaming: true }], true), 'unavailable')
  assert.equal(getStreamingState([], true), 'unavailable')
  assert.equal(getStreamingState([{ uuid: 'unknown' }]), 'unavailable')
  assert.equal(getStreamingState([{ uuid: 'unknown' }, { streaming: true }]), 'streaming')
})

test('an unsuccessful or malformed list must not become a zero-device empty state', () => {
  for (const payload of [
    {},
    { status: false, named_certs: [] },
    { status: 'true', named_certs: null },
  ]) {
    assert.throws(() => parseHomeClients(payload))
  }
  assert.deepEqual(parseHomeClients({ status: 'true', named_certs: [] }), [])
})

test('paired clients without observed streams do not produce fake recent connections', () => {
  assert.deepEqual(
    updateRecentConnections([], [{ uuid: 'a', name: 'TV', streaming: false }], 1000),
    [],
  )
})

test('observed streaming activity persists, is renamed from authoritative clients, and disappears when unpaired', () => {
  const records = updateRecentConnections([], [{ uuid: 'a', name: 'TV', streaming: true }], 1000)
  assert.deepEqual(records, [{ uuid: 'a', name: 'TV', lastSeen: 1000 }])
  assert.deepEqual(
    updateRecentConnections(records, [{ uuid: 'a', name: 'Renamed TV', streaming: false }], 2000),
    [{ uuid: 'a', name: 'Renamed TV', lastSeen: 1000 }],
  )
  assert.deepEqual(updateRecentConnections(records, [], 2000), [])
})

test('invalid cached records cannot render invalid timestamps or unpaired device names', () => {
  const records = [
    null,
    { uuid: 'a', lastSeen: 'bad' },
    { uuid: 'a', lastSeen: 5000 },
    { uuid: 'unknown', lastSeen: 500 },
  ]
  assert.deepEqual(updateRecentConnections(records, [{ uuid: 'a' }], 1000), [])
})

test('recent list is bounded and ordered by observed activity', () => {
  const clients = Array.from({ length: 10 }, (_, i) => ({ uuid: String(i), name: String(i) }))
  const records = clients.map((client, i) => ({ ...client, lastSeen: i + 1 }))
  const result = updateRecentConnections(records, clients, 100)
  assert.equal(result.length, 8)
  assert.equal(result[0].uuid, '9')
  assert.equal(result[7].uuid, '2')
})

test('blocked or corrupted storage does not prevent a usable home page', () => {
  assert.deepEqual(readRecentConnections({ getItem: () => '{broken' }, 'key'), [])
  assert.deepEqual(
    readRecentConnections(
      {
        getItem: () => {
          throw new Error('Blocked')
        },
      },
      'key',
    ),
    [],
  )
  assert.doesNotThrow(() =>
    writeRecentConnections(
      {
        setItem: () => {
          throw new Error('Quota')
        },
      },
      'key',
      [],
    ),
  )
})

test('relative seen labels tier by recency and fall back for old or invalid records', () => {
  const zh = getHomeCopy('zh').time
  const en = getHomeCopy('en').time
  const now = 86400000 * 30
  assert.equal(formatRelativeSeen(now - 30 * 1000, zh, now), '刚刚')
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

test('home copy separates first use from history empty in both languages', () => {
  for (const locale of ['zh', 'zh_TW', 'en', 'en_US', 'ja']) {
    const copy = getHomeCopy(locale)
    assert.notEqual(copy.noDevices, copy.noHistory)
    assert.ok(copy.support.entry)
  }
  assert.equal(getHomeCopy('zh-TW').recent, '最近連線')
})

test('virtual display navigation targets only an embedded trusted native shell', () => {
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
    openNativeDisplaySettings(
      { ...shell, location: { ancestorOrigins: ['https://evil.example'] } },
      { referrer: '' },
    ),
    false,
  )
  assert.equal(openNativeDisplaySettings({ ...shell, isTauri: false }, { referrer: '' }), false)
  const standalone = { isTauri: true }
  standalone.parent = standalone
  assert.equal(openNativeDisplaySettings(standalone, {}), false)
})

test('host-side session history fills the recent list without local observation', () => {
  const now = 86400000 * 30
  const clients = [
    { uuid: 'tv', name: 'TV', streaming: false, lastSessionEnded: now - 86400000 * 2.2 },
    { uuid: 'pc', name: 'PC', streaming: false, lastSessionEnded: 0 },
  ]
  assert.deepEqual(updateRecentConnections([], clients, now), [
    { uuid: 'tv', name: 'TV', lastSeen: now - 86400000 * 2.2 },
  ])
})

test('backend fact and local observation keep whichever is newer', () => {
  const now = 86400000 * 30
  const client = { uuid: 'tv', name: 'TV', streaming: false }
  const local = [{ uuid: 'tv', name: 'TV', lastSeen: now - 3600000 }]
  assert.equal(
    updateRecentConnections(local, [{ ...client, lastSessionEnded: now - 86400000 * 5 }], now)[0]
      .lastSeen,
    now - 3600000,
  )
  assert.equal(
    updateRecentConnections([], [{ ...client, lastSessionEnded: now - 60000 }], now)[0].lastSeen,
    now - 60000,
  )
})

test('future or empty backend timestamps cannot invent or shift history', () => {
  const now = 86400000 * 30
  const client = { uuid: 'tv', name: 'TV', streaming: false }
  assert.deepEqual(updateRecentConnections([], [{ ...client, lastSessionEnded: now + 999999 }], now), [
    { uuid: 'tv', name: 'TV', lastSeen: now },
  ])
  assert.deepEqual(updateRecentConnections([], [client], now), [])
  assert.deepEqual(updateRecentConnections([], [{ ...client, lastSessionEnded: 'bad' }], now), [])
})

test('stale or corrupt sibling stores are swept without touching the active one', () => {
  const now = Date.now()
  const stores = new Map([
    ['sunshine-recent-connections:old-host:7840', JSON.stringify([{ uuid: 'a', lastSeen: now - 86400000 * 120 }])],
    ['sunshine-recent-connections:lan:7840', JSON.stringify([{ uuid: 'a', lastSeen: now - 86400000 }])],
    ['sunshine-recent-connections:junk:7840', '{broken'],
    ['unrelated:key', 'keep me'],
  ])
  const storage = {
    length: stores.size,
    key: (index) => [...stores.keys()][index],
    getItem: (key) => stores.get(key) ?? null,
    removeItem: (key) => {
      stores.delete(key)
    },
  }
  pruneRecentConnectionStores(storage, 'sunshine-recent-connections:lan:7840', now)
  assert.ok(!stores.has('sunshine-recent-connections:old-host:7840'), 'stale store swept')
  assert.ok(!stores.has('sunshine-recent-connections:junk:7840'), 'corrupt store swept')
  assert.ok(stores.has('sunshine-recent-connections:lan:7840'), 'fresh sibling kept')
  assert.ok(stores.has('unrelated:key'), 'unrelated keys untouched')
})

test('pruning tolerates storage that refuses enumeration', () => {
  assert.doesNotThrow(() =>
    pruneRecentConnectionStores(
      {
        get length() {
          throw new Error('Blocked')
        },
        key: () => null,
        getItem: () => null,
        removeItem: () => {},
      },
      'sunshine-recent-connections:lan:7840',
    ),
  )
})
