import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'

import { normalizeAppDisplayProfile } from '../utils/appDisplayProfile.js'

test('app without a display scheme keeps no display fields at all', () => {
  const normalized = normalizeAppDisplayProfile({
    name: 'Desktop',
    'display-target': '',
    'display-device-prep': 'ensure_only_display',
    'display-resolution-mode': 'no_operation',
    'display-obsolete-option': 'legacy',
  })

  assert.deepEqual(normalized, { name: 'Desktop' })
})

test('app display scheme fields are preserved', () => {
  const normalized = normalizeAppDisplayProfile({
    name: 'Game',
    'display-target': 'virtual',
    'display-device-prep': 'no_operation',
    'display-resolution-mode': 'client',
    'display-refresh-rate-mode': 'client',
  })

  assert.equal(normalized['display-target'], 'virtual')
  assert.equal(normalized['display-device-prep'], 'no_operation')
  assert.equal(normalized['display-resolution-mode'], 'client')
  assert.equal(normalized['display-refresh-rate-mode'], 'client')
})

test('invalid mode values are cleared', () => {
  const normalized = normalizeAppDisplayProfile({
    name: 'Game',
    'display-target': 'physical',
    'display-resolution-mode': 'bogus',
    'display-refresh-rate-mode': 'manual',
  })

  assert.equal(normalized['display-resolution-mode'], '')
  assert.equal(normalized['display-refresh-rate-mode'], '')
})

test('invalid display-target clears the whole profile', () => {
  const normalized = normalizeAppDisplayProfile({
    name: 'Game',
    'display-target': 'bogus',
    'display-device-prep': 'ensure_primary',
    'display-resolution-mode': 'client',
    'display-hdr': 'on',
  })

  assert.deepEqual(normalized, { name: 'Game' })
})

test('refresh rate no_operation is dropped (no per-app refresh gate in the 0-change design)', () => {
  const normalized = normalizeAppDisplayProfile({
    name: 'Game',
    'display-target': 'physical',
    'display-refresh-rate-mode': 'no_operation',
  })

  assert.equal(normalized['display-refresh-rate-mode'], '')
})

test('invalid fixed values are cleared', () => {
  const normalized = normalizeAppDisplayProfile({
    name: 'Game',
    'display-target': 'virtual',
    'display-resolution': 'not-a-resolution',
    'display-refresh-rate': 'abc',
  })

  assert.equal(normalized['display-resolution'], '')
  assert.equal(normalized['display-refresh-rate'], '')
})

test('fixed values survive when well formed', () => {
  const normalized = normalizeAppDisplayProfile({
    name: 'Game',
    'display-target': 'virtual',
    'display-resolution': '2560x1440',
    'display-refresh-rate': '60',
  })

  assert.equal(normalized['display-resolution'], '2560x1440')
  assert.equal(normalized['display-refresh-rate'], '60')
})

test('fixed HDR state must be on/off', () => {
  const on = normalizeAppDisplayProfile({ name: 'G', 'display-target': 'virtual', 'display-hdr': 'on' })
  const off = normalizeAppDisplayProfile({ name: 'G', 'display-target': 'virtual', 'display-hdr': 'off' })
  const bad = normalizeAppDisplayProfile({ name: 'G', 'display-target': 'virtual', 'display-hdr': 'forced' })

  assert.equal(on['display-hdr'], 'on')
  assert.equal(off['display-hdr'], 'off')
  assert.equal(bad['display-hdr'], '')
})

test('display-output-name only kept for a physical target', () => {
  const physical = normalizeAppDisplayProfile({
    name: 'G',
    'display-target': 'physical',
    'display-output-name': '\\\\.\\DISPLAY1',
  })
  const virtual = normalizeAppDisplayProfile({
    name: 'G',
    'display-target': 'virtual',
    'display-output-name': '\\\\.\\DISPLAY1',
  })

  assert.equal(physical['display-output-name'], '\\\\.\\DISPLAY1')
  assert.equal(virtual['display-output-name'], undefined)
})

test('app editor reuses the shared display components', async () => {
  const source = await readFile(new URL('../components/AppEditor.vue', import.meta.url), 'utf8')

  assert.match(source, /import DisplayPreparationPicker from/)
  assert.match(source, /<DisplayPreparationPicker/)
  assert.match(source, /import NewDisplayOutputSelector from/)
  assert.match(source, /<NewDisplayOutputSelector/)
  assert.match(source, /import DisplayRuleRadioGroup from/)
  assert.match(source, /<DisplayRuleRadioGroup/)
})
