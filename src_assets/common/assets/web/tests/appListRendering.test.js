import assert from 'node:assert/strict'
import test from 'node:test'
import { useApps } from '../composables/useApps.js'

test('reordering preserves application identity and updates original indices', () => {
  const list = useApps()
  list.apps.value = [{ name: 'Same name' }, { name: 'Same name' }, { name: 'Other' }]
  const original = [...list.apps.value]
  const keys = original.map(list.getAppRenderKey)
  assert.equal(new Set(keys).size, 3)

  list.reorderApps([...list.filteredAppsWithIndex.value].reverse())

  assert.deepEqual(list.apps.value, [...original].reverse())
  assert.equal(list.apps.value[0], original[2])
  assert.deepEqual(list.apps.value.map(list.getAppRenderKey), [...keys].reverse())
  assert.deepEqual(list.filteredAppsWithIndex.value.map(({ index }) => index), [0, 1, 2])
})

test('search and selection cannot overwrite the application list with a subset', () => {
  const list = useApps()
  list.apps.value = [{ name: 'Alpha' }, { name: 'Beta' }, { name: 'Gamma' }]
  const original = [...list.apps.value]
  const subset = [list.filteredAppsWithIndex.value[1]]

  list.searchQuery.value = 'Beta'
  list.reorderApps(subset)
  assert.deepEqual(list.apps.value, original)

  list.searchQuery.value = ''
  list.selectionMode.value = true
  list.reorderApps(subset)
  assert.deepEqual(list.apps.value, original)

  list.selectionMode.value = false
  list.reorderApps(subset)
  assert.deepEqual(list.apps.value, original)
})
