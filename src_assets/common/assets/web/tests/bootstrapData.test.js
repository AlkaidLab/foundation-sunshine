import test from 'node:test'
import assert from 'node:assert/strict'
import {
  getBootstrapConfig,
  getBootstrapLocale,
  resetBootstrapDataCache,
} from '../config/bootstrapData.js'

const jsonResponse = (data, { ok = true, status = 200 } = {}) => ({
  ok,
  status,
  json: async () => data,
})

test('deduplicates concurrent bootstrap config requests', async () => {
  resetBootstrapDataCache()
  const originalFetch = globalThis.fetch
  let calls = 0
  globalThis.fetch = async () => {
    calls += 1
    return jsonResponse({ locale: 'zh', platform: 'windows' })
  }

  try {
    const [first, second] = await Promise.all([getBootstrapConfig(), getBootstrapConfig()])
    assert.equal(calls, 1)
    assert.strictEqual(first, second)
  } finally {
    globalThis.fetch = originalFetch
    resetBootstrapDataCache()
  }
})

test('keeps config and locale bootstrap requests independent', async () => {
  resetBootstrapDataCache()
  const originalFetch = globalThis.fetch
  const calls = []
  globalThis.fetch = async (url) => {
    calls.push(url)
    return jsonResponse({ locale: url.endsWith('configLocale') ? 'zh_TW' : 'zh' })
  }

  try {
    const [config, locale] = await Promise.all([getBootstrapConfig(), getBootstrapLocale()])
    assert.equal(config.locale, 'zh')
    assert.equal(locale.locale, 'zh_TW')
    assert.deepEqual(calls, ['/api/config', '/api/configLocale'])
  } finally {
    globalThis.fetch = originalFetch
    resetBootstrapDataCache()
  }
})

test('retries bootstrap data after a failed request', async () => {
  resetBootstrapDataCache()
  const originalFetch = globalThis.fetch
  let calls = 0
  globalThis.fetch = async () => {
    calls += 1
    return calls === 1
      ? jsonResponse({ error: 'temporary failure' }, { ok: false, status: 503 })
      : jsonResponse({ locale: 'en' })
  }

  try {
    await assert.rejects(getBootstrapConfig(), /temporary failure/)
    assert.equal((await getBootstrapConfig()).locale, 'en')
    assert.equal(calls, 2)
  } finally {
    globalThis.fetch = originalFetch
    resetBootstrapDataCache()
  }
})
