import test from 'node:test'
import assert from 'node:assert/strict'
import { codecRows } from '../utils/codecMatrix.js'

const row = (rows, id) => rows.find((r) => r.id === id)

test('the probe result decides: offered, 10-bit, and the codec in use', () => {
  const rows = codecRows({ hevc_mode: 0, av1_mode: 0, active_hevc_mode: 3, active_av1_mode: 2 }, 'hevc')
  assert.deepEqual([row(rows, 'h264').offered, row(rows, 'h264').now], ['yes', false])
  assert.deepEqual([row(rows, 'hevc').offered, row(rows, 'hevc').deep, row(rows, 'hevc').now, row(rows, 'hevc').probed], ['yes', 'yes', true, true])
  assert.deepEqual([row(rows, 'av1').offered, row(rows, 'av1').deep, row(rows, 'av1').now], ['yes', 'no', false])
})

test('a codec the probe found unsupported is not offered whatever the setting says', () => {
  const rows = codecRows({ hevc_mode: 3, av1_mode: 3, active_hevc_mode: 1, active_av1_mode: 1 })
  assert.deepEqual([row(rows, 'hevc').offered, row(rows, 'hevc').deep, row(rows, 'av1').offered], ['no', 'no', 'no'])
})

test('without a probe result the configured modes are read, and automatic is shown as automatic', () => {
  const auto = codecRows({ hevc_mode: 0, av1_mode: 0 })
  assert.deepEqual([row(auto, 'hevc').offered, row(auto, 'hevc').probed], ['auto', false])
  const fixed = codecRows({ hevc_mode: 3, av1_mode: 1 })
  assert.deepEqual([row(fixed, 'hevc').offered, row(fixed, 'hevc').deep, row(fixed, 'av1').offered], ['yes', 'yes', 'no'])
  // active_* = 0 means the host has not probed yet
  assert.equal(row(codecRows({ hevc_mode: 1, active_hevc_mode: 0 }), 'hevc').offered, 'no')
})

test('the session codec is matched however the host spells it', () => {
  assert.equal(row(codecRows({}, 'H.264'), 'h264').now, true)
  assert.equal(row(codecRows({}, 'av1'), 'av1').now, true)
  assert.equal(row(codecRows({}, ''), 'hevc').now, false)
})
