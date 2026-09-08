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
    pin.editingStates['client-1'] = true

    assert.equal(await pin.saveClient('client-1'), false)
    assert.equal(pin.editingStates['client-1'], true)
    assert.equal(pin.originalValues['client-1'], undefined)
  } finally {
    globalThis.fetch = originalFetch
  }
})

test('save keeps all clients editable when a 200 response reports failure', async () => {
  const originalFetch = globalThis.fetch
  globalThis.fetch = async () => jsonResponse({ status: false, error: 'rejected' })

  try {
    const pin = usePin()
    pin.config.value = { clients: '[]' }
    pin.clients.value = [
      { uuid: 'client-1', name: 'First', hdrProfile: 'first.icc' },
      { uuid: 'client-2', name: 'Second', hdrProfile: 'second.icm' },
    ]
    pin.editingStates['client-1'] = true
    pin.editingStates['client-2'] = true

    assert.equal(await pin.save(), false)
    assert.equal(pin.editingStates['client-1'], true)
    assert.equal(pin.editingStates['client-2'], true)
    assert.deepEqual(Object.keys(pin.originalValues), [])
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
