// SPDX-License-Identifier: GPL-3.0-only
// The checks of meta.test.js of foundation-ui that read this product's settings data (the search index and the label tables): they live here because the data does.
import assert from 'node:assert/strict'
import test from 'node:test'
import './helpers/foundationUi.mjs'
const { SEARCH_INDEX, LABELS } = await import('../config/settingsData/index.js')
const { SETTING_META } = await import('foundation-ui/settings/index.js')

test('every setting of the search index has a tier and a control, and nothing else is in the registry', () => {
  const ids = SEARCH_INDEX.map((e) => e.id)
  const missing = ids.filter((id) => !SETTING_META[id])
  assert.deepEqual(missing, [], `settings without a registry entry: ${missing.join(', ')}`)
  const stray = Object.keys(SETTING_META).filter((id) => !ids.includes(id))
  assert.deepEqual(stray, [], `registry entries without a setting: ${stray.join(', ')}`)
})
