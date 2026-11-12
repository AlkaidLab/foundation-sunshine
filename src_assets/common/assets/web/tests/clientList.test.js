import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { clientKind, filterClients, groupClients, kindOptions, clientTexts } from '../utils/clientList.js'

const clients = [
  { name: 'iPhone 15 Pro' }, { name: 'Steam Deck' }, { name: 'Living room TV' }, { name: 'linux-test-host MacBook' }, { name: 'iPad Air' }, { name: 'Pixel 9' }, { name: 'Raspberry' },
]

test('a device kind comes from its name; no match is other', () => {
  assert.equal(clientKind('iPhone 15 Pro'), 'phone')
  assert.equal(clientKind('iPad Air'), 'tablet')
  assert.equal(clientKind('linux-test-host MacBook'), 'mac')
  assert.equal(clientKind('DESKTOP-4K2L'), 'computer')
  assert.equal(clientKind('Living room TV'), 'tv')
  assert.equal(clientKind('Steam Deck'), 'handheld')
  assert.equal(clientKind('Quest 3'), 'other')
  assert.equal(clientKind(''), 'other')
})

test('the system the client reports decides what it can be, the name refines it', () => {
  // a phone whose name says nothing is a phone when the host saw Android; a tablet is a tablet by its name or its model word
  assert.equal(clientKind({ name: 'vivo X100', os: 'android' }), 'phone')
  assert.equal(clientKind({ name: 'K60', os: 'android' }), 'phone')
  assert.equal(clientKind({ name: 'Xiaomi Pad 6', os: 'android' }), 'tablet')
  assert.equal(clientKind({ name: 'HUAWEI MatePad', os: 'android' }), 'tablet')
  assert.equal(clientKind({ name: 'Galaxy Tab S9', os: 'android' }), 'tablet')
  assert.equal(clientKind({ name: 'Living room', os: 'tvos' }), 'tv')
  assert.equal(clientKind({ name: 'Bedroom TV', os: 'android' }), 'tv')
  assert.equal(clientKind({ name: 'Sky', os: 'ios' }), 'phone')
  assert.equal(clientKind({ name: 'iPad Pro', os: 'ios' }), 'tablet')
  assert.equal(clientKind({ name: 'Sky', os: 'macos' }), 'mac')
  assert.equal(clientKind({ name: 'Sky', os: 'windows' }), 'computer')
  assert.equal(clientKind({ name: 'Deck', os: 'linux' }), 'computer')
  assert.equal(clientKind({ name: 'Steam Deck', os: 'linux' }), 'handheld')
  assert.equal(clientKind({ name: 'ROG Ally', os: 'windows' }), 'handheld')
  // a name cannot turn the system into another one: an Android phone named "iPad" is no iPadOS device, a Mac named "TV" is a Mac
  assert.equal(clientKind({ name: 'My PC', os: 'android' }), 'phone')
  assert.equal(clientKind({ name: 'TV', os: 'macos' }), 'mac')
  // an unknown or empty system falls back to the name alone
  assert.equal(clientKind({ name: 'Pixel 9', os: '' }), 'phone')
  assert.equal(clientKind({ name: 'Pixel 9', os: null }), 'phone')
  assert.equal(clientKind({ name: 'Zako' }), 'other')
})

test('the filter keeps the kind and the text, the groups keep a fixed order and leave empty kinds out', () => {
  assert.equal(filterClients(clients, 'all', '').length, 7)
  assert.deepEqual(filterClients(clients, 'phone', '').map((c) => c.name), ['iPhone 15 Pro', 'Pixel 9'])
  assert.deepEqual(filterClients(clients, 'all', ' IPA ').map((c) => c.name), ['iPad Air'])
  assert.deepEqual(groupClients(clients).map((g) => g.kind), ['phone', 'tablet', 'mac', 'handheld', 'tv', 'other'])
  assert.deepEqual(groupClients([{ name: 'Pixel 9' }]).map((g) => g.kind), ['phone'])
})

test('the options carry the counts and only the kinds that exist', () => {
  const options = kindOptions(clients, clientTexts('en'))
  assert.equal(options[0].value, 'all')
  assert.equal(options[0].label, 'All 7')
  assert.deepEqual(options.slice(1).map((o) => o.value), ['phone', 'tablet', 'mac', 'handheld', 'tv', 'other'])
  assert.equal(options.find((o) => o.value === 'phone').label, 'Phones 2')
  assert.equal(clientTexts('zh-CN').pinPair, 'PIN 配对')
})

test('a chosen device size shows in the row even when it is the default (medium, a tablet); a defaulted one does not', () => {
  const view = readFileSync(new URL('../views/Pin.vue', import.meta.url), 'utf8')
  const pin = readFileSync(new URL('../composables/usePin.js', import.meta.url), 'utf8')
  assert.match(view, /client\.deviceSize && \(client\.deviceSizeSet \|\| client\.deviceSize !== DEFAULT_DEVICE_SIZE\)/)
  assert.match(pin, /merged\.deviceSizeSet = !!merged\.deviceSize\n\s+if \(!merged\.deviceSize\)/)
  assert.match(pin, /deviceSizeSet: _sizeSet/, 'the flag is not saved with the client')
})
