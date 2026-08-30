import test from 'node:test'
import assert from 'node:assert/strict'

import { useQrPair } from '../composables/useQrPair.js'

test('remote connection status blocks mutations until the initial read completes', async (t) => {
  const originalFetch = globalThis.fetch
  t.after(() => {
    globalThis.fetch = originalFetch
  })

  let finishRequest
  globalThis.fetch = () => new Promise((resolve) => {
    finishRequest = () => resolve(new Response(JSON.stringify({
      enabled: true,
      running: true,
      available: true,
    }), { status: 200, headers: { 'Content-Type': 'application/json' } }))
  })

  const pairing = useQrPair()
  const loading = pairing.loadRemoteConnectStatus()
  assert.equal(pairing.remoteConnectBusy.value, true)

  finishRequest()
  await loading
  assert.equal(pairing.remoteConnectBusy.value, false)
  assert.equal(pairing.remoteConnectEnabled.value, true)
})
