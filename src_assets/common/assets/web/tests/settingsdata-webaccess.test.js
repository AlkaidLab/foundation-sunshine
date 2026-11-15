// SPDX-License-Identifier: GPL-3.0-only
// The checks of webaccess.test.js of foundation-ui that read this product's settings data (the search index and the label tables): they live here because the data does.
import assert from 'node:assert/strict'
import test from 'node:test'
import './helpers/foundationUi.mjs'
const { SEARCH_INDEX, LABELS } = await import('../config/settingsData/index.js')
const { buildSettingsEntries, searchEntries } = await import('foundation-ui/settings/search.js')
const { labelOf } = await import('foundation-ui/settings/index.js')

test('search finds the access scope by 局域网, 公网, 手机, 绑定, IP, 访问, LAN and remote, and names it 访问范围', () => {
  const entries = (locale) => buildSettingsEntries({ index: SEARCH_INDEX, label: (k) => labelOf(locale, k), altLabel: (k) => labelOf(locale === 'zh' ? 'en' : 'zh', k) })
  const zh = entries('zh')
  const ids = (q) => searchEntries(zh, q).map((e) => e.id)
  for (const q of ['局域网', '公网', '手机', '访问', 'LAN', 'remote', '访问范围']) assert.ok(ids(q).includes('origin_web_ui_allowed'), q)
  for (const q of ['绑定', 'IP', '手机', '绑定地址']) assert.ok(ids(q).includes('bind_address'), q)
  assert.equal(labelOf('zh', 'config.origin_web_ui_allowed'), '访问范围')
  assert.ok(searchEntries(entries('en'), 'phone').some((e) => e.id === 'origin_web_ui_allowed'))
})
