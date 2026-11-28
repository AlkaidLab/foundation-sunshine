import test from 'node:test'
import assert from 'node:assert/strict'
import { SETTING_META as sharedMeta, homesOf as sharedHomesOf } from 'foundation-ui/settings/index.js'
import { DISPLAY_TREE, SETTING_META, homeOf, isChanged } from '../config/settingsData/hostSettings.js'
import { differences, importPlan } from '../utils/configBackup.js'

test('the EDID option has one advanced home without changing shared settings definitions', () => {
  const key = 'vdd_edid_native_resolution'
  assert.equal(sharedMeta[key], undefined)
  assert.deepEqual(sharedHomesOf(key), [])
  assert.deepEqual(SETTING_META[key], { tier: 3, control: 'sheet', def: 'default' })
  assert.deepEqual(homeOf(key), { place: 'virtual', section: 'advanced' })
  const sections = DISPLAY_TREE.find(place => place.place === 'virtual').sections
  assert.equal(sections.filter(section => section.keys.includes(key)).length, 1)
  for (const [name, meta] of Object.entries(sharedMeta)) assert.equal(SETTING_META[name], meta)
})

test('EDID choices participate in backup differences and imports with the original default', () => {
  const key = 'vdd_edid_native_resolution'
  for (const value of [undefined, null, '', 'default']) assert.equal(isChanged(key, value), false)
  const [entry] = differences({ [key]: '2736x1824' })
  assert.equal(entry.key, key)
  assert.equal(entry.def, 'default')
  assert.equal(entry.place.sub, 'virtual')
  const plan = importPlan({ [key]: 'default' }, { [key]: '2736x1824' })
  assert.deepEqual(plan.changes, [{ key, from: '2736x1824', to: 'default' }])
})
