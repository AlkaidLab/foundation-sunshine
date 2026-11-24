import test from 'node:test'
import assert from 'node:assert/strict'
import { appsEntries, applyAppEntries, revertAppEntries, syncRevertedApps, useAppsPending } from '../composables/pendingApps.js'
import { nextTick, ref } from 'vue'
import { resetPending, usePending } from 'foundation-ui/pending'

const T = { add: 'Add', del: 'Delete', mod: 'Change', added: 'Added', deleted: 'Deleted', changed: 'Changed', existing: 'Existing', was: 'Before', order: 'Order', oldOrder: 'Old', newOrder: 'New' }
const app = (name, extra = {}) => ({ name, cmd: '', ...extra })
const original = [app('Desktop'), app('Steam'), app('Forza')]

test('undoing a saved order updates the open list and keeps later command edits', async () => {
  resetPending()
  const apps = ref([app('Steam'), app('Desktop'), app('Forza')])
  const originalApps = ref([app('Steam'), app('Desktop'), app('Forza')])
  const stop = useAppsPending({ apps, originalApps, locale: () => 'en', loaded: ref(true) })
  try {
    apps.value[2].cmd = 'new-command'
    await nextTick()
    syncRevertedApps(original)
    await nextTick()
    assert.deepEqual(apps.value.map((a) => a.name), ['Desktop', 'Steam', 'Forza'])
    assert.equal(apps.value[2].cmd, 'new-command')
    assert.deepEqual(usePending().entries('apps').map((e) => e.key), ['mod:Forza'])
  } finally {
    stop()
    usePending().sync('apps', [])
    resetPending()
  }
})

test('saving updates the baseline without discarding edits made during the save', async () => {
  resetPending()
  const pending = usePending()
  let finishSave
  pending.define('apps', { save: () => new Promise((resolve) => { finishSave = resolve }) })
  const apps = ref(original.map((a) => ({ ...a })))
  const originalApps = ref(original.map((a) => ({ ...a })))
  const stop = useAppsPending({ apps, originalApps, locale: () => 'en', loaded: ref(true) })
  try {
    apps.value = [apps.value[1], apps.value[0], apps.value[2]]
    await nextTick()
    const saving = pending.saveSource('apps')
    apps.value[2].cmd = 'new-command'
    finishSave({ ok: true })
    await saving
    await nextTick()
    assert.deepEqual(originalApps.value.map((a) => a.name), ['Steam', 'Desktop', 'Forza'])
    assert.deepEqual(pending.entries('apps').map((e) => e.key), ['mod:Forza'])
  } finally {
    stop()
    pending.sync('apps', [])
    resetPending()
  }
})

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
