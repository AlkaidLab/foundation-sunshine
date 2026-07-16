import test from 'node:test'
import assert from 'node:assert/strict'

import { apiFetch, apiJson, apiPostJson } from '../utils/apiFetch.js'

const withMockFetch = async (handler, run) => {
  const originalFetch = globalThis.fetch
  globalThis.fetch = handler

  try {
    await run()
  } finally {
    globalThis.fetch = originalFetch
  }
}

test('apiPostJson stringifies plain object bodies and sets JSON content type', async () => {
  let capturedUrl
  let capturedOptions

  await withMockFetch(
    async (url, options) => {
      capturedUrl = url
      capturedOptions = options
      return new Response(JSON.stringify({ ok: true }), { status: 200 })
    },
    async () => {
      const data = await apiPostJson('/api/example', { enabled: true })

      assert.deepEqual(data, { ok: true })
      assert.equal(capturedUrl, '/api/example')
      assert.equal(capturedOptions.method, 'POST')
      assert.equal(capturedOptions.headers.get('Content-Type'), 'application/json')
      assert.equal(capturedOptions.body, JSON.stringify({ enabled: true }))
    }
  )
})

test('apiJson throws HTTP errors with server error messages when available', async () => {
  await withMockFetch(
    async () => new Response(JSON.stringify({ error: 'bad request' }), { status: 400 }),
    async () => {
      await assert.rejects(() => apiJson('/api/fail'), /bad request/)
    }
  )
})

test('apiJson preserves successful business failure payloads for callers to interpret', async () => {
  await withMockFetch(
    async () => new Response(JSON.stringify({ status: 'false', error: 'pairing failed' }), { status: 200 }),
    async () => {
      const data = await apiJson('/api/business-fail')

      assert.deepEqual(data, { status: 'false', error: 'pairing failed' })
    }
  )
})

test('apiFetch returns raw responses for text and header consumers', async () => {
  await withMockFetch(
    async () => new Response('log text', { status: 200, headers: { 'X-Log-Size': '8' } }),
    async () => {
      const response = await apiFetch('/api/logs')

      assert.equal(response.headers.get('X-Log-Size'), '8')
      assert.equal(await response.text(), 'log text')
    }
  )
})
