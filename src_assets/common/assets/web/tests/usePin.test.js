import test from 'node:test'
import assert from 'node:assert/strict'

import { formatHdrNits, isColorProfileFile, usePin } from '../composables/usePin.js'

const jsonResponse = (data, { ok = true, status = 200 } = {}) => ({
  ok,
  status,
  json: async () => data,
})

test('recognizes ICC and ICM color profiles case-insensitively', () => {
  assert.equal(isColorProfileFile('display.icc'), true)
  assert.equal(isColorProfileFile('display.ICM'), true)
  assert.equal(isColorProfileFile('display.IcC'), true)
  assert.equal(isColorProfileFile('display.icc.backup'), false)
  assert.equal(isColorProfileFile('displayXicc'), false)
  assert.equal(isColorProfileFile(null), false)
})

test('an empty authoritative client list clears stale devices after unpairing', async () => {
  const originalFetch = globalThis.fetch
  globalThis.fetch = async () => jsonResponse({ status: true, named_certs: [] })
  try {
    const pin = usePin()
    pin.clients.value = [{ uuid: 'removed', name: 'Old device' }]
    await pin.refreshClients()
    assert.deepEqual(pin.clients.value, [])
  } finally { globalThis.fetch = originalFetch }
})

test('pairing permissions and names override stale per-device display configuration', async () => {
  const originalFetch = globalThis.fetch
  globalThis.fetch = async () => jsonResponse({ status: true, named_certs: [{ uuid: 'client-1', name: 'Renamed', enabled: false, streaming: false }] })
  try {
    const pin = usePin()
    pin.config.value = { clients: JSON.stringify([{ uuid: 'client-1', name: 'Old name', enabled: true, streaming: true, hdrProfile: 'display.icc' }]) }
    await pin.refreshClients()
    assert.equal(pin.clients.value[0].enabled, false)
    assert.equal(pin.clients.value[0].streaming, false)
    assert.equal(pin.clients.value[0].name, 'Renamed')
    assert.equal(pin.clients.value[0].hdrProfile, 'display.icc')
  } finally { globalThis.fetch = originalFetch }
})

test('client permissions are not updated locally when the backend rejects the change', async () => {
  const originalFetch = globalThis.fetch
  globalThis.fetch = async (url, options) => {
    assert.equal(url, '/api/clients/update')
    assert.deepEqual(JSON.parse(options.body), { uuid: 'client-1', enabled: false })
    return jsonResponse({ status: false, error: 'Unable to persist' })
  }
  try {
    const pin = usePin()
    pin.clients.value = [{ uuid: 'client-1', enabled: true, streaming: true }]
    assert.equal(await pin.setClientEnabled('client-1', false), false)
    assert.equal(pin.clients.value[0].enabled, true)
    assert.equal(pin.clients.value[0].streaming, true)
    assert.equal(pin.updating.value.size, 0)
    assert.equal(pin.clientError.value, 'Unable to persist')
  } finally { globalThis.fetch = originalFetch }
})

test('disabling preserves the paired device and HDR configuration', async () => {
  const originalFetch = globalThis.fetch
  globalThis.fetch = async () => jsonResponse({ status: true, enabled: false })
  try {
    const pin = usePin()
    pin.clients.value = [{ uuid: 'client-1', enabled: true, streaming: true, hdrProfile: 'display.icc', hdrBrightnessMaxNits: 1200 }]
    assert.equal(await pin.setClientEnabled('client-1', false), true)
    assert.equal(pin.clients.value.length, 1)
    assert.equal(pin.clients.value[0].enabled, false)
    assert.equal(pin.clients.value[0].streaming, false)
    assert.equal(pin.clients.value[0].hdrProfile, 'display.icc')
    assert.equal(pin.clients.value[0].hdrBrightnessMaxNits, 1200)
  } finally { globalThis.fetch = originalFetch }
})

test('saving display settings never serializes authorization or streaming status', async () => {
  const originalFetch = globalThis.fetch
  let stored
  globalThis.fetch = async (url, options) => {
    assert.equal(url, '/api/clients/list')
    stored = JSON.parse(JSON.parse(options.body).clients)[0]
    return jsonResponse({ status: true })
  }
  try {
    const pin = usePin()
    pin.config.value = { clients: '[]' }
    pin.clients.value = [{ uuid: 'client-1', name: 'Device', enabled: false, streaming: false, hdrProfile: '' }]
    assert.equal(await pin.saveClient('client-1'), true)
    assert.equal(Object.hasOwn(stored, 'enabled'), false)
    assert.equal(Object.hasOwn(stored, 'streaming'), false)
  } finally { globalThis.fetch = originalFetch }
})

test('cancelling display edits cannot restore an old connection permission', () => {
  const pin = usePin()
  pin.clients.value = [{ uuid: 'client-1', name: 'Device', enabled: true, streaming: true }]
  pin.startEdit('client-1')
  pin.clients.value[0].name = 'Unsaved'
  pin.clients.value[0].enabled = false
  pin.clients.value[0].streaming = false
  pin.cancelEdit('client-1')
  assert.equal(pin.clients.value[0].name, 'Device')
  assert.equal(pin.clients.value[0].enabled, false)
  assert.equal(pin.clients.value[0].streaming, false)
})

test('formats runtime HDR luminance without exposing float noise', () => {
  assert.equal(formatHdrNits(701.9262084960938), '701.9')
  assert.equal(formatHdrNits(155.9835968017578), '156.0')
  assert.equal(formatHdrNits(0.0010000000474974513, 4, true), '0.001')
  assert.equal(formatHdrNits(0.0001, 4, true), '0.0001')
  assert.equal(formatHdrNits(Number.NaN), '—')
})

test('loads installed profiles from the authenticated Core endpoint', async () => {
  const originalFetch = globalThis.fetch
  const originalWindow = globalThis.window
  globalThis.window = {}
  globalThis.fetch = async (url) => {
    assert.equal(url, '/api/color-profiles')
    return jsonResponse({
      status: true,
      supported: true,
      profiles: ['display.icc', 'legacy.ICM', 'ignore.txt'],
    })
  }

  try {
    const pin = usePin()
    await pin.loadColorProfiles()
    assert.equal(pin.hasIccFileList.value, true)
    assert.deepEqual(pin.hdrProfileList.value, ['display.icc', 'legacy.ICM'])
  } finally {
    globalThis.fetch = originalFetch
    globalThis.window = originalWindow
  }
})

test('saveClient keeps the client editable when a 200 response reports failure', async () => {
  const originalFetch = globalThis.fetch
  globalThis.fetch = async () => jsonResponse({ status: 'false', error: 'rejected' })

  try {
    const pin = usePin()
    pin.config.value = { clients: '[]' }
    pin.clients.value = [{ uuid: 'client-1', name: 'Client', hdrProfile: 'display.icc' }]
    pin.startEdit('client-1')
    pin.clients.value[0].hdrProfile = 'edited.icc'

    assert.equal(await pin.saveClient('client-1'), false)
    assert.equal(pin.config.value.clients, '[]')
    assert.equal(pin.clientError.value, 'rejected')
    pin.cancelEdit('client-1')
    assert.equal(pin.clients.value[0].hdrProfile, 'display.icc')
  } finally {
    globalThis.fetch = originalFetch
  }
})

test('unpair all preserves clients when a 200 response reports failure', async () => {
  const originalFetch = globalThis.fetch
  globalThis.fetch = async () => jsonResponse({ status: false, error: 'rejected' })

  try {
    const pin = usePin()
    pin.config.value = { clients: '[]' }
    pin.clients.value = [
      { uuid: 'client-1', name: 'First', hdrProfile: 'first.icc' },
      { uuid: 'client-2', name: 'Second', hdrProfile: 'second.icm' },
    ]
    assert.equal(await pin.unpairAll(), false)
    assert.equal(pin.clients.value.length, 2)
    assert.equal(pin.clientError.value, 'rejected')
  } finally {
    globalThis.fetch = originalFetch
  }
})

test('partial device saves keep the persisted name when cancelling unsaved display edits', async (t) => {
  for (const httpFailure of [false, true]) {
    await t.test(httpFailure ? 'HTTP error' : 'application error', async (t) => {
      t.mock.method(globalThis, 'fetch', async (url) => url === '/api/clients/rename'
        ? jsonResponse({ status: true })
        : jsonResponse({ status: false, error: 'Display settings rejected' }, { ok: !httpFailure, status: httpFailure ? 500 : 200 }))
      const pin = usePin((key, { error }) => `${key}: ${error}`)
      pin.config.value = { clients: '[]' }
      pin.clients.value = [{ uuid: 'client-1', name: 'Old name', hdrProfile: 'old.icc', enabled: true }]
      pin.startEdit('client-1')
      pin.clients.value[0].name = 'Saved name'
      pin.clients.value[0].hdrProfile = 'unsaved.icc'

      assert.equal(await pin.saveClient('client-1'), false)
      assert.equal(pin.clientError.value, 'connections.rename_partial: Display settings rejected')
      assert.equal(pin.clients.value[0].hdrProfile, 'unsaved.icc')
      assert.equal(pin.config.value.clients, '[]')
      pin.cancelEdit('client-1')
      assert.equal(pin.clients.value[0].name, 'Saved name')
      assert.equal(pin.clients.value[0].hdrProfile, 'old.icc')
      assert.equal(pin.clients.value[0].enabled, true)
      assert.equal(pin.saving.value, false)
    })
  }
})

test('retrying a partial device save preserves the draft and does not rename twice', async (t) => {
  let renames = 0
  let saves = 0
  t.mock.method(globalThis, 'fetch', async (url) => {
    if (url === '/api/clients/rename') { renames++; return jsonResponse({ status: true }) }
    return jsonResponse(++saves === 1 ? { status: false, error: 'Retry me' } : { status: true })
  })
  const pin = usePin()
  pin.config.value = { clients: '[]' }
  pin.clients.value = [{ uuid: 'client-1', name: 'Old name', hdrProfile: 'old.icc' }]
  pin.startEdit('client-1')
  pin.clients.value[0].name = 'Saved name'
  pin.clients.value[0].hdrProfile = 'draft.icc'
  assert.equal(await pin.saveClient('client-1'), false)
  assert.equal(await pin.saveClient('client-1'), true)
  assert.equal(renames, 1)
  assert.equal(pin.clientError.value, '')
  assert.equal(JSON.parse(pin.config.value.clients)[0].hdrProfile, 'draft.icc')
})

test('a failed rename leaves the original edit snapshot unchanged', async (t) => {
  t.mock.method(globalThis, 'fetch', async (url) => {
    assert.equal(url, '/api/clients/rename')
    return jsonResponse({ status: false, error: 'Rename rejected' })
  })
  const pin = usePin()
  pin.config.value = { clients: '[]' }
  pin.clients.value = [{ uuid: 'client-1', name: 'Old name', hdrProfile: 'old.icc' }]
  pin.startEdit('client-1')
  pin.clients.value[0].name = 'Unsaved name'
  assert.equal(await pin.saveClient('client-1'), false)
  assert.equal(pin.clientError.value, 'Rename rejected')
  pin.cancelEdit('client-1')
  assert.equal(pin.clients.value[0].name, 'Old name')
})

test('unpair all clears the authoritative list without restarting the host', async () => {
  const originalFetch = globalThis.fetch
  const requests = []
  globalThis.fetch = async (url) => {
    requests.push(url)
    if (url === '/api/clients/unpair-all') return jsonResponse({ status: true })
    assert.equal(url, '/api/clients/list')
    return jsonResponse({ status: true, named_certs: [] })
  }
  try {
    const pin = usePin()
    pin.clients.value = [{ uuid: 'client-1', name: 'Client' }]
    assert.equal(await pin.unpairAll(), true)
    assert.deepEqual(pin.clients.value, [])
    assert.deepEqual(requests, ['/api/clients/unpair-all', '/api/clients/list'])
  } finally {
    globalThis.fetch = originalFetch
  }
})

test('saveClient persists manual HDR brightness without runtime status', async () => {
  const originalFetch = globalThis.fetch
  let requestBody
  globalThis.fetch = async (_url, options) => {
    requestBody = JSON.parse(options.body)
    return jsonResponse({ status: true })
  }

  try {
    const pin = usePin()
    pin.config.value = { clients: '[]' }
    pin.clients.value = [{
      uuid: 'client-1',
      name: 'Client',
      hdrBrightnessMode: 'manual',
      hdrBrightnessMaxNits: 800,
      hdrBrightnessMinNits: 0.01,
      hdrBrightnessMaxFullFrameNits: 400,
      hdrBrightnessRuntime: { active: true, source: 'client_report' },
    }]

    assert.equal(await pin.saveClient('client-1'), true)
    const stored = JSON.parse(requestBody.clients)[0]
    assert.equal(stored.hdrBrightnessMode, 'manual')
    assert.equal(stored.hdrBrightnessMaxNits, 800)
    assert.equal(stored.hdrBrightnessRuntime, undefined)
  } finally {
    globalThis.fetch = originalFetch
  }
})

test('refreshClients merges runtime HDR status and initializes automatic defaults', async () => {
  const originalFetch = globalThis.fetch
  globalThis.fetch = async () => jsonResponse({
    status: 'true',
    named_certs: [{
      uuid: 'client-1',
      name: 'Client',
      hdrBrightnessRuntime: {
        active: true,
        source: 'client_report',
        maxNits: 1200,
        minNits: 0.005,
        maxFullFrameNits: 600,
      },
    }],
  })

  try {
    const pin = usePin()
    pin.config.value = { clients: '[]' }
    await pin.refreshClients()

    assert.equal(pin.clients.value[0].hdrBrightnessMode, 'auto')
    assert.equal(pin.clients.value[0].hdrBrightnessRuntime.source, 'client_report')
    assert.equal(pin.clients.value[0].hdrBrightnessRuntime.maxNits, 1200)
  } finally {
    globalThis.fetch = originalFetch
  }
})
