import test from 'node:test'
import assert from 'node:assert/strict'

import { ds4HintOf, ds4HintReason } from '../utils/gamepadHint.js'

test('a DualSense the host selected DS4 for names the host as the reason', () => {
  const session = { gamepad_type: 'ds4', gamepad_dualsense_identity: true }
  assert.equal(ds4HintReason(session, 'ds4'), 'host')
  assert.deepEqual(ds4HintOf([session], 'ds4'), { reason: 'host', key: 'config.gamepad_ds4_hint_host' })
})

test('an identified DualSense on another host mode points at the client or app selection', () => {
  const session = { gamepad_type: 'ds4', gamepad_dualsense_identity: true }
  assert.equal(ds4HintReason(session, 'auto'), 'client')
  assert.equal(ds4HintReason(session, ''), 'client')
  assert.deepEqual(ds4HintOf([session], 'auto'), { reason: 'client', key: 'config.gamepad_ds4_hint_client' })
})

test('a pad the client did not identify is the legacy-client reason, whatever the host mode', () => {
  const session = { gamepad_type: 'ds4', gamepad_dualsense_identity: false }
  assert.equal(ds4HintReason(session, 'auto'), 'legacy')
  assert.equal(ds4HintReason(session, 'ds4'), 'legacy')
  assert.deepEqual(ds4HintOf([session]), { reason: 'legacy', key: 'config.gamepad_ds4_hint_legacy' })
})

test('no hint without a DS4 run: another type, no pad or no session', () => {
  assert.equal(ds4HintReason({ gamepad_type: 'ds5', gamepad_dualsense_identity: true }, 'ds4'), '')
  assert.equal(ds4HintReason({ gamepad_type: 'x360', gamepad_dualsense_identity: false }, 'x360'), '')
  assert.equal(ds4HintReason({ gamepad_type: '', gamepad_dualsense_identity: false }), '')
  assert.equal(ds4HintReason(null, 'ds4'), '')
  assert.equal(ds4HintOf([], 'ds4'), null)
  assert.equal(ds4HintOf(null), null)
})

test('the first session that runs a DS4 answers, also among several', () => {
  const streaming = [
    { gamepad_type: 'ds5', gamepad_dualsense_identity: true },
    { gamepad_type: 'ds4', gamepad_dualsense_identity: false },
    { gamepad_type: 'ds4', gamepad_dualsense_identity: true },
  ]
  assert.deepEqual(ds4HintOf(streaming, 'ds4'), { reason: 'legacy', key: 'config.gamepad_ds4_hint_legacy' })
})
