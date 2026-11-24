import test from 'node:test'
import assert from 'node:assert/strict'
import { countApps, encoderLabel, summariseClients } from '../utils/shellLive.js'

test('clients are counted from the named certificate list and streaming ones are those enabled and streaming', () => {
  const s = summariseClients({ status: true, named_certs: [{ name: 'a', enabled: true, streaming: true }, { name: 'b', enabled: false, streaming: true }, { name: 'c' }] })
  assert.equal(s.total, 3)
  assert.equal(s.streaming, 1)
  assert.equal(summariseClients({ status: true }), null)
  assert.equal(summariseClients(null), null)
})

test('apps are counted from /api/apps and an answer without a list gives null', () => {
  assert.equal(countApps({ apps: [{}, {}] }), 2)
  assert.equal(countApps([{}]), 1)
  assert.equal(countApps({}), null)
})

test('encoder names are only respelled; an empty answer stays empty', () => {
  assert.equal(encoderLabel('nvenc'), 'NVENC')
  assert.equal(encoderLabel('quicksync'), 'Quick Sync')
  assert.equal(encoderLabel('something-new'), 'something-new')
  assert.equal(encoderLabel(''), '')
  assert.equal(encoderLabel(undefined), '')
})
