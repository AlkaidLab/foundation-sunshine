import assert from 'node:assert/strict'
import test from 'node:test'

import { getPreferredEncoderTab } from '../config/encoderTabs.js'

const WINDOWS_TABS = ['nv', 'qsv', 'amd', 'sw']

test('active encoder takes priority over configured encoder and adapter names', () => {
  assert.equal(
    getPreferredEncoderTab({
      activeEncoder: 'amdvce',
      configuredEncoder: 'nvenc',
      selectedAdapter: 'NVIDIA GeForce RTX 4080',
      availableTabIds: WINDOWS_TABS,
    }),
    'amd',
  )
})

test('configured encoder is used when runtime probing has no result', () => {
  assert.equal(
    getPreferredEncoderTab({
      configuredEncoder: 'quicksync',
      selectedAdapter: 'AMD Radeon 780M Graphics',
      availableTabIds: WINDOWS_TABS,
    }),
    'qsv',
  )
})

test('selected adapter is preferred over the enumerated adapter order', () => {
  assert.equal(
    getPreferredEncoderTab({
      selectedAdapter: 'NVIDIA GeForce RTX 4070 Laptop GPU',
      adapters: [{ name: 'AMD Radeon 780M Graphics' }, { name: 'NVIDIA GeForce RTX 4070 Laptop GPU' }],
      availableTabIds: WINDOWS_TABS,
    }),
    'nv',
  )
})

test('enumerated adapter names provide a fallback for automatic GPU selection', () => {
  assert.equal(
    getPreferredEncoderTab({
      adapters: [{ name: 'Intel(R) Arc(TM) Graphics' }],
      availableTabIds: WINDOWS_TABS,
    }),
    'qsv',
  )
})

test('unavailable encoder tabs are skipped before adapter matching', () => {
  assert.equal(
    getPreferredEncoderTab({
      activeEncoder: 'amdvce',
      selectedAdapter: 'NVIDIA GeForce RTX 3060',
      availableTabIds: ['nv', 'sw'],
    }),
    'nv',
  )
})

test('macOS falls back to VideoToolbox when no encoder metadata is available', () => {
  assert.equal(
    getPreferredEncoderTab({
      platform: 'macos',
      availableTabIds: ['vt', 'sw'],
    }),
    'vt',
  )
})

test('unknown hardware preserves the first available encoder tab fallback', () => {
  assert.equal(
    getPreferredEncoderTab({
      adapters: [{ name: 'Microsoft Basic Render Driver' }],
      availableTabIds: WINDOWS_TABS,
    }),
    'nv',
  )
  assert.equal(getPreferredEncoderTab(), null)
})
