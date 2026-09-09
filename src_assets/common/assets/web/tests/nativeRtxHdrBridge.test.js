import assert from 'node:assert/strict'
import test from 'node:test'

import {
  createNativeRtxHdrBridge,
  isTrustedRtxHdrBridgeMessage,
  resolveControlPanelOrigin,
} from '../utils/nativeRtxHdrBridge.js'

test('RTX HDR bridge accepts only known control-panel origins', () => {
  assert.equal(resolveControlPanelOrigin('https://tauri.localhost/frame', ''), 'https://tauri.localhost')
  assert.equal(resolveControlPanelOrigin('https://evil.example/frame', ''), '')
})

test('RTX HDR bridge rejects forged source, origin, and source markers', () => {
  const parent = {}
  const valid = {
    source: parent,
    origin: 'https://tauri.localhost',
    data: { source: 'sunshine-control-panel' },
  }
  assert.equal(isTrustedRtxHdrBridgeMessage(valid, parent, valid.origin), true)
  assert.equal(isTrustedRtxHdrBridgeMessage({ ...valid, source: {} }, parent, valid.origin), false)
  assert.equal(isTrustedRtxHdrBridgeMessage({ ...valid, origin: 'https://evil.example' }, parent, valid.origin), false)
  assert.equal(isTrustedRtxHdrBridgeMessage({ ...valid, data: { source: 'evil' } }, parent, valid.origin), false)
})

test('standalone WebUI never advertises or opens the native manager', () => {
  const messages = []
  const standalone = {
    parent: null,
    location: { ancestorOrigins: [] },
  }
  standalone.parent = standalone
  standalone.postMessage = (message) => messages.push(message)
  const bridge = createNativeRtxHdrBridge({
    windowObject: standalone,
    documentObject: { referrer: '' },
    onAvailability: () => assert.fail('standalone bridge must stay unavailable'),
  })
  assert.equal(bridge.requestContext(), false)
  assert.equal(bridge.openManager(), false)
  assert.deepEqual(messages, [])
})

test('embedded WebUI opens only after a trusted capability response', () => {
  const messages = []
  const parent = { postMessage: (message, origin) => messages.push({ message, origin }) }
  const embedded = { parent, location: { ancestorOrigins: ['https://tauri.localhost'] } }
  let available = false
  const bridge = createNativeRtxHdrBridge({
    windowObject: embedded,
    documentObject: { referrer: '' },
    onAvailability: (value) => { available = value },
  })
  assert.equal(bridge.openManager(), false)
  assert.equal(bridge.requestContext(), true)
  bridge.handleMessage({
    source: parent,
    origin: 'https://tauri.localhost',
    data: { type: 'native-rtx-hdr-context', source: 'sunshine-control-panel', available: true },
  })
  assert.equal(available, true)
  assert.equal(bridge.openManager(), true)
  assert.equal(messages.at(-1).message.type, 'native-rtx-hdr-open-request')
  assert.equal(messages.at(-1).origin, 'https://tauri.localhost')
})
