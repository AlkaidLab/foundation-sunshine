// SPDX-License-Identifier: GPL-3.0-only
// The checks of destinations.test.js of foundation-ui that read this product's settings data (the search index and the label tables): they live here because the data does.
import assert from 'node:assert/strict'
import test from 'node:test'
import './helpers/foundationUi.mjs'
const { SEARCH_INDEX, LABELS } = await import('../config/settingsData/index.js')
const { SETTINGS_CATEGORIES, SETTINGS_GROUPS, categoryOfRow, dualsenseEntries, labelOf, settingsGroups } = await import('foundation-ui/settings/index.js')

const zh = (key) => labelOf('zh', key)

const en = (key) => labelOf('en', key)

test('DualSense is in no list, no rail destination and no page of the settings', () => {
  assert.ok(!SETTINGS_GROUPS.flatMap((g) => g.categories).includes('dualsense'))
  assert.ok(!Object.keys(SETTINGS_CATEGORIES).includes('dualsense'))
  for (const locale of Object.keys(LABELS)) {
    const items = settingsGroups({ label: (key) => labelOf(locale, key) }).flatMap((g) => g.items)
    for (const item of items) {
      assert.ok(!/dualsense/i.test(`${item.id} ${item.route} ${item.category}`), `${locale}: ${item.id}`)
      assert.ok(!/dualsense/i.test(item.label), `${locale}: label of ${item.id}`)
    }
  }
  assert.equal(categoryOfRow('dualsense'), '')
  assert.ok(!SEARCH_INDEX.some((e) => /dualsense/.test(e.id)), 'no row opens a DualSense page')
})

test('the way to a DualSense setting is 输入 › 手柄 › card, in both languages, and it opens the sub-page of the gamepad page', () => {
  const keys = [
    { id: 3, key: 'ds5.haptics.hd.gain', label: 'Haptics gain', group: 1, tier: 'common' },
    { id: 24, key: 'ds5.trigger.adaptive.enabled', label: 'Trigger', group: 3, tier: 'common' },
    { id: 36, key: 'ds5.gamepad.stick.left.deadzoneInner', label: 'Left dead zone', group: 4, tier: 'advanced' },
    { id: 1, key: 'ds5.profile.preset', label: 'Preset', group: 0, tier: 'common' },
  ]
  const zhRows = dualsenseEntries({ keys, label: zh, locale: 'zh' })
  assert.equal(zhRows[0].path, '输入 › 手柄 › 触觉与震动')
  assert.equal(zhRows[1].path, '输入 › 手柄 › 自适应扳机')
  assert.equal(zhRows[2].path, '输入 › 手柄 › 摇杆')
  assert.equal(zhRows[3].path, '输入 › 手柄')
  assert.equal(dualsenseEntries({ keys, label: en, locale: 'en' })[0].path, 'Input › Gamepad › Haptics and rumble')
  for (const entry of zhRows) assert.equal(entry.route, 'config')
  assert.deepEqual(zhRows[0].params, { tab: 'input', sub: 'ds5-haptics', focus: 'ds5:ds5.haptics.hd.gain' })
  assert.deepEqual(zhRows[2].params.sub, 'ds5-g4')
  assert.ok(!('sub' in zhRows[3].params), 'the preset row is on the gamepad page itself')
  assert.ok(!zhRows.some((e) => /DualSense 调校|DualSense/.test(e.path)), 'no page called DualSense in the way')
})
