import test from 'node:test'
import assert from 'node:assert/strict'

import { useRemoteConnect } from '../composables/useRemoteConnect.js'

const jsonResponse = (data, status = 200) => new Response(JSON.stringify(data), {
  status,
  headers: { 'Content-Type': 'application/json' },
})

test('remote connection status blocks mutations until the initial read completes', async (t) => {
  const originalFetch = globalThis.fetch
  t.after(() => {
    globalThis.fetch = originalFetch
  })

  let finishRequest
  globalThis.fetch = () => new Promise((resolve) => {
    finishRequest = () => resolve(jsonResponse({
      enabled: true,
      running: true,
      available: true,
    }))
  })

  const remoteConnect = useRemoteConnect()
  const loading = remoteConnect.loadStatus()
  assert.equal(remoteConnect.busy.value, true)

  finishRequest()
  await loading
  assert.equal(remoteConnect.busy.value, false)
  assert.equal(remoteConnect.enabled.value, true)
})

test('remote connection mutations send JSON and apply the returned status', async (t) => {
  const originalFetch = globalThis.fetch
  t.after(() => {
    globalThis.fetch = originalFetch
  })

  const requests = []
  globalThis.fetch = async (url, options) => {
    requests.push({ url, options })
    if (url === '/api/remote-connect/reset') {
      return jsonResponse({ enabled: false, running: false, available: true })
    }
    return jsonResponse({ enabled: true, running: true, available: true })
  }

  const remoteConnect = useRemoteConnect()
  assert.equal(await remoteConnect.setEnabled(true), true)
  assert.equal(remoteConnect.enabled.value, true)
  assert.equal(remoteConnect.running.value, true)
  assert.deepEqual(JSON.parse(requests[0].options.body), { enabled: true })
  assert.equal(requests[0].options.method, 'POST')

  assert.equal(await remoteConnect.reset(), true)
  assert.equal(remoteConnect.enabled.value, false)
  assert.equal(remoteConnect.running.value, false)
  assert.deepEqual(JSON.parse(requests[1].options.body), {})
})

test('remote connection mutation keeps the server error and reports failure', async (t) => {
  const originalFetch = globalThis.fetch
  t.after(() => {
    globalThis.fetch = originalFetch
  })

  globalThis.fetch = async () => jsonResponse({
    status: false,
    enabled: false,
    running: false,
    available: true,
    error: 'runtime unavailable',
  })

  const remoteConnect = useRemoteConnect()
  assert.equal(await remoteConnect.setEnabled(true), false)
  assert.equal(remoteConnect.enabled.value, false)
  assert.equal(remoteConnect.error.value, 'runtime unavailable')
})
