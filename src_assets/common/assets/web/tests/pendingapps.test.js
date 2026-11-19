import test from 'node:test'
import assert from 'node:assert/strict'
import { appsEntries, applyAppEntries, revertAppEntries } from '../composables/pendingApps.js'

const T = { add: 'Add', del: 'Delete', mod: 'Change', added: 'Added', deleted: 'Deleted', changed: 'Changed', existing: 'Existing', was: 'Before', order: 'Order', oldOrder: 'Old', newOrder: 'New' }
const app = (name, extra = {}) => ({ name, cmd: '', ...extra })
const original = [app('Desktop'), app('Steam'), app('Forza')]

test('every added, removed and changed application is one entry and an unchanged list has none', () => {
  assert.deepEqual(appsEntries(original, JSON.parse(JSON.stringify(original)), T), [])
  const now = [app('Desktop'), app('Steam', { cmd: 'steam://open' }), app('Elden Ring')]
  assert.deepEqual(appsEntries(original, now, T).map((e) => e.key), ['add:Elden Ring', 'del:Forza', 'mod:Steam'])
})

test('a new order of the applications that stay is one entry of its own', () => {
  const entries = appsEntries(original, [app('Steam'), app('Desktop'), app('Forza')], T)
  assert.deepEqual(entries.map((e) => e.key), ['order'])
})

test('the entries apply to the list the host has now, so the list can be saved from another page', () => {
  const now = [app('Desktop'), app('Steam', { cmd: 'steam://open' }), app('Elden Ring')]
  const entries = appsEntries(original, now, T)
  assert.deepEqual(applyAppEntries(original, entries), now)
})

test('undo of one entry puts back only that application; the saved copy can be written back whole', () => {
  const now = [app('Desktop'), app('Steam', { cmd: 'steam://open' }), app('Elden Ring')]
  const entries = appsEntries(original, now, T)
  const keepMod = revertAppEntries(now, entries.filter((e) => e.key === 'del:Forza' || e.key === 'add:Elden Ring'))
  assert.deepEqual(keepMod.map((a) => a.name), ['Desktop', 'Steam', 'Forza'])
  assert.equal(keepMod[1].cmd, 'steam://open')
  assert.deepEqual(revertAppEntries(now, entries), original)
})
