import test from 'node:test'
import assert from 'node:assert/strict'

import { useVddStatus } from '../composables/useVddStatus.js'

test('VDD status falls back to the authenticated Core endpoint in a browser', async (t) => {
  const originalWindow = globalThis.window
  const originalFetch = globalThis.fetch
  t.after(() => {
    globalThis.window = originalWindow
    globalThis.fetch = originalFetch
  })

  globalThis.window = {}
  globalThis.fetch = async (url) => {
    assert.equal(url, '/api/vdd/status')
    return new Response(JSON.stringify({
      status: true,
      state: 'not_installed',
      installed: false,
      running: false,
    }), { status: 200, headers: { 'Content-Type': 'application/json' } })
  }

  const vdd = useVddStatus()
  await vdd.refreshVddStatus()

  assert.equal(vdd.canManageVdd.value, false)
  assert.equal(vdd.vddStatus.value.state, 'not_installed')
  assert.equal(vdd.vddStatus.value.version_supported, false)
  assert.equal(vdd.vddStatus.value.version_match, null)
  assert.equal(vdd.vddReady.value, false)
})

test('VDD status preserves real desktop version probe results', async (t) => {
  const originalWindow = globalThis.window
  t.after(() => {
    globalThis.window = originalWindow
  })

  globalThis.window = {
    vddDriver: {
      getStatus: async () => ({
        state: 'ready',
        installed: true,
        running: true,
        installed_version: '1.2.3',
        bundled_version: '1.2.3',
        version_match: true,
      }),
    },
  }

  const vdd = useVddStatus()
  await vdd.refreshVddStatus()

  assert.equal(vdd.vddStatus.value.version_supported, true)
  assert.equal(vdd.vddStatus.value.installed_version, '1.2.3')
  assert.equal(vdd.vddStatus.value.version_match, true)
})

test('VDD status prefers the desktop bridge and verifies installation', async (t) => {
  const originalWindow = globalThis.window
  const originalFetch = globalThis.fetch
  t.after(() => {
    globalThis.window = originalWindow
    globalThis.fetch = originalFetch
  })

  let installed = false
  globalThis.fetch = async () => {
    throw new Error('Core endpoint should not be used when the desktop bridge is available')
  }
  globalThis.window = {
    vddDriver: {
      getStatus: async () => ({
        state: installed ? 'ready' : 'not_installed',
        installed,
        running: installed,
      }),
      install: async () => {
        installed = true
        return { state: 'ready', installed: true, running: true }
      },
    },
  }

  const vdd = useVddStatus()
  await vdd.refreshVddStatus()
  assert.equal(vdd.vddReady.value, false)

  await vdd.installVdd()
  assert.equal(vdd.canManageVdd.value, true)
  assert.equal(vdd.vddReady.value, true)
  assert.equal(vdd.vddStatus.value.state, 'ready')
})

test('VDD installation cancellation keeps state visible and allows retry', async (t) => {
  const originalWindow = globalThis.window
  t.after(() => {
    globalThis.window = originalWindow
  })

  let attempts = 0
  globalThis.window = {
    vddDriver: {
      getStatus: async () => ({ state: 'not_installed', installed: false, running: false }),
      install: async () => {
        attempts += 1
        if (attempts === 1) throw new Error('UAC request was cancelled')
        return { state: 'ready', installed: true, running: true }
      },
    },
  }

  const vdd = useVddStatus()
  await vdd.refreshVddStatus()

  await assert.rejects(vdd.installVdd(), /cancelled/)
  assert.equal(vdd.vddStatus.value.state, 'not_installed')
  assert.equal(vdd.vddStatusError.value, 'UAC request was cancelled')
  assert.equal(vdd.vddInstalling.value, false)

  await vdd.installVdd()
  assert.equal(vdd.vddReady.value, true)
  assert.equal(vdd.vddStatusError.value, '')
})
