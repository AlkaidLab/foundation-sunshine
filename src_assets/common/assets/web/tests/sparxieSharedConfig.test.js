import test from 'node:test'
import assert from 'node:assert/strict'
import { PATHS, createClient, normalizeStatus } from '../services/sparxieClient.js'

test('the status says whose config is used, with the component there or not', () => {
  for (const installed of [true, false]) assert.equal(normalizeStatus({ installed, sharedConfigDir: 'D:/Apps/Sparxie' }).sharedConfigDir, 'D:/Apps/Sparxie')
  assert.equal(normalizeStatus({ installed: true }).sharedConfigDir, '')
})

test('sharing a config posts the folder to the host', () => {
  assert.equal(PATHS.sharedConfig, '/api/sparxie/shared-config')
  assert.equal(typeof createClient().selectSharedConfig, 'function')
})
