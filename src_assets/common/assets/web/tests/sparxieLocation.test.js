import test from 'node:test'
import assert from 'node:assert/strict'
import { createClient, normalizeStatus, PATHS } from '../services/sparxieClient.js'

test('the status says which install is used and whether administrators alone can change it', () => {
  const open = normalizeStatus({ installed: true, external: true, externalDir: 'D:\\Apps\\Sparxie', adminOnly: false, adminOnlyBlocker: 'D:\\Apps' })
  assert.deepEqual([open.external, open.externalDir, open.adminOnly, open.adminOnlyBlocker], [true, 'D:\\Apps\\Sparxie', false, 'D:\\Apps'])
  // a chosen folder that lost its Sparxie is still shown, so the choice can be changed
  const gone = normalizeStatus({ installed: false, external: true, externalDir: 'D:\\Apps\\Sparxie' })
  assert.deepEqual([gone.installed, gone.external, gone.externalDir], [false, true, 'D:\\Apps\\Sparxie'])
  // a host without these members changes nothing
  assert.equal(normalizeStatus({ installed: true }).adminOnly, true)
})

test('choosing an install posts the folder; an empty one goes back to the component', () => {
  assert.equal(PATHS.location, '/api/sparxie/location')
  assert.equal(typeof createClient().selectLocation, 'function')
})
