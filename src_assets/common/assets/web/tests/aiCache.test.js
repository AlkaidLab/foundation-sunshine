import test from 'node:test'
import assert from 'node:assert/strict'

import { __aiCacheTestUtils, createAiCache } from '../utils/aiCache.js'

test('createAiCache stores values under stable payload keys', () => {
  __aiCacheTestUtils.clearMemoryStores()
  const cache = createAiCache('unit-cache', { version: 'test' })
  const keyA = cache.makeKey({ b: 2, a: 1 })
  const keyB = cache.makeKey({ a: 1, b: 2 })

  assert.equal(keyA, keyB)
  assert.equal(cache.get(keyA), undefined)

  cache.set(keyA, { ok: true })
  assert.deepEqual(cache.get(keyB), { ok: true })
})

test('createAiCache preserves cached null values', () => {
  __aiCacheTestUtils.clearMemoryStores()
  const cache = createAiCache('unit-null-cache', { version: 'test' })
  const key = cache.makeKey({ name: 'missing-cover' })

  cache.set(key, null)

  assert.equal(cache.get(key), null)
})

test('createAiCache falls back to memory when localStorage access throws', () => {
  __aiCacheTestUtils.clearMemoryStores()
  const originalDescriptor = Object.getOwnPropertyDescriptor(globalThis, 'localStorage')

  Object.defineProperty(globalThis, 'localStorage', {
    configurable: true,
    get() {
      throw new Error('storage denied')
    },
  })

  try {
    const cache = createAiCache('unit-denied-cache', { version: 'test' })
    const key = cache.makeKey({ name: 'restricted-storage' })
    cache.set(key, { ok: true })
    assert.deepEqual(cache.get(key), { ok: true })
  } finally {
    if (originalDescriptor) {
      Object.defineProperty(globalThis, 'localStorage', originalDescriptor)
    } else {
      delete globalThis.localStorage
    }
  }
})
