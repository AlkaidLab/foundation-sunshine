// SPDX-License-Identifier: GPL-3.0-only
// The checks of virtualdisplay.test.js of foundation-ui that read this product's settings data (the search index and the label tables): they live here because the data does.
import assert from 'node:assert/strict'
import test from 'node:test'
import './helpers/foundationUi.mjs'
const { SEARCH_INDEX, LABELS } = await import('../config/settingsData/index.js')
const { VDD_KEYS, VDD_SEARCH_KEYS } = await import('foundation-ui/settings/index.js')

test('every key of the control is a setting the registry or the index knows', () => {
  const ids = new Set(SEARCH_INDEX.map((e) => e.id))
  for (const [key] of VDD_KEYS) assert.ok(ids.has(key) || VDD_SEARCH_KEYS.some(([k]) => k === key), key)
  assert.equal(new Set(VDD_SEARCH_KEYS.map(([k]) => k)).size, VDD_SEARCH_KEYS.length)
})
