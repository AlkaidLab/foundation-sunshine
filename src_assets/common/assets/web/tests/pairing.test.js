import test from 'node:test'
import assert from 'node:assert/strict'
import { brightnessErrors, canSaveDevice, canSubmitPairing, clientState } from '../utils/pairing.js'

const fields = [{ key: 'peak', min: 1, max: 10000 }, { key: 'min', min: 0, max: 100 }]

test('pairing needs a full numeric PIN and a non-blank name', () => {
  assert.equal(canSubmitPairing('1234', 'Phone'), true)
  assert.equal(canSubmitPairing('123', 'Phone'), false)
  assert.equal(canSubmitPairing('12a4', 'Phone'), false)
  assert.equal(canSubmitPairing('1234', '   '), false)
  assert.equal(canSubmitPairing('1234', undefined), false)
  assert.equal(canSubmitPairing('123456', 'x', 6), true)
})

test('client state: disabled wins over streaming, streaming over allowed', () => {
  assert.equal(clientState({ enabled: false, streaming: true }), 'disabled')
  assert.equal(clientState({ enabled: true, streaming: true }), 'streaming')
  assert.equal(clientState({ enabled: true }), 'allowed')
  assert.equal(clientState({}), 'allowed')
})

test('brightness fields are checked only in manual mode', () => {
  assert.deepEqual(brightnessErrors({ hdrBrightnessMode: 'auto', peak: 99999 }, fields), [])
  assert.deepEqual(brightnessErrors({ hdrBrightnessMode: 'manual', peak: 1000, min: 0.001 }, fields), [])
  assert.deepEqual(brightnessErrors({ hdrBrightnessMode: 'manual', peak: 0, min: 500 }, fields), ['peak', 'min'])
  assert.deepEqual(brightnessErrors({ hdrBrightnessMode: 'manual', peak: '', min: 1 }, fields), ['peak'])
})

test('device settings can be saved with a name and valid fields', () => {
  assert.equal(canSaveDevice({ name: 'A', hdrBrightnessMode: 'auto' }, fields), true)
  assert.equal(canSaveDevice({ name: ' ', hdrBrightnessMode: 'auto' }, fields), false)
  assert.equal(canSaveDevice({ name: 'A', hdrBrightnessMode: 'manual', peak: 0, min: 0 }, fields), false)
  assert.equal(canSaveDevice(null, fields), false)
})
