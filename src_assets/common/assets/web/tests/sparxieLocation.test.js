import test from 'node:test'
import assert from 'node:assert/strict'
import { createClient, normalizeStatus, PATHS } from '../services/sparxieClient.js'

test('the status says which install is used', () => {
  const chosen = normalizeStatus({ installed: true, external: true, externalDir: 'D:\\Apps\\Sparxie' })
  assert.deepEqual([chosen.external, chosen.externalDir], [true, 'D:\\Apps\\Sparxie'])
  // a chosen folder that lost its Sparxie is still shown, so the choice can be changed
  const gone = normalizeStatus({ installed: false, external: true, externalDir: 'D:\\Apps\\Sparxie' })
  assert.deepEqual([gone.installed, gone.external, gone.externalDir], [false, true, 'D:\\Apps\\Sparxie'])
  // a host without these members changes nothing
  assert.equal(normalizeStatus({ installed: true }).external, false)
})

test('choosing an install posts the folder; an empty one goes back to the component', () => {
  assert.equal(PATHS.location, '/api/sparxie/location')
  assert.equal(typeof createClient().selectLocation, 'function')
})
