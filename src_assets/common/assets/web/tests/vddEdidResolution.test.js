import test from 'node:test'
import assert from 'node:assert/strict'
import { edidResolutionOptions, validEdidResolutionSelection } from '../utils/vddEdidResolution.js'

test('EDID options track configured custom resolutions and deduplicate normalized sizes', () => {
  const resolutions = ['1920x1080', '3440x1440', '2037X1109', '3440x1440']
  const options = edidResolutionOptions(resolutions, ['60'])
  assert.deepEqual(options.map(option => option.value), ['default', '1920x1080', '3440x1440', '2037x1109'])
  assert.ok(options.every(option => !option.reason))
  resolutions.push('2560x1600')
  assert.ok(edidResolutionOptions(resolutions, ['60']).some(option => option.value === '2560x1600'))
})

test('original default remains available without 1080p in the configured list', () => {
  assert.equal(validEdidResolutionSelection('default', [], []), true)
  assert.equal(edidResolutionOptions([], [])[0].value, 'default')
  assert.equal(validEdidResolutionSelection('1920x1080', ['3440x1440'], ['60']), false)
})

test('removed selections stay visible as disabled and require an explicit new choice', () => {
  const selected = edidResolutionOptions(['1920x1080'], ['60'], '3440x1440').at(-1)
  assert.deepEqual(selected, { value: '3440x1440', reason: 'not_advertised' })
  assert.equal(validEdidResolutionSelection('3440x1440', ['1920x1080'], ['60']), false)
})

test('EDID dimension and clock limits disable unsafe modes without truncation', () => {
  const sizes = ['5120x1440', '4096x2160', '0x1080', 'not-a-mode', '3840x2160', '4095x4095', '1x1']
  const options = edidResolutionOptions(sizes, ['120'])
  assert.ok(options.slice(1).every(option => option.reason === 'not_encodable'))
  assert.equal(validEdidResolutionSelection('3840x2160', sizes, ['59.94']), true)
  assert.equal(validEdidResolutionSelection('4095x4095', sizes, ['30']), true)
  assert.equal(validEdidResolutionSelection('3840x2160', sizes, ['120', '60']), true)
})

test('invalid frame rates cannot make an EDID mode available', () => {
  const fps = [null, undefined, true, '', 'bad', '1e2', NaN, Infinity, '0', '481']
  assert.equal(validEdidResolutionSelection('3440x1440', ['3440x1440'], fps), false)
})

test('frame rates discarded by the existing config serializer cannot enable a choice', () => {
  assert.equal(validEdidResolutionSelection('3440x1440', ['3440x1440'], ['24', '29.97']), false)
  assert.equal(validEdidResolutionSelection('3440x1440', ['3440x1440'], ['24', '30']), true)
})
