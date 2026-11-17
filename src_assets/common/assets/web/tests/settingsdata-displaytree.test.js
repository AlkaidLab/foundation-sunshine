// SPDX-License-Identifier: GPL-3.0-only
// The checks of displaytree.test.js of foundation-ui that read this product's settings data (the search index and the label tables): they live here because the data does.
import assert from 'node:assert/strict'
import test from 'node:test'
import './helpers/foundationUi.mjs'
const { SEARCH_INDEX, LABELS } = await import('../config/settingsData/index.js')
const { DISPLAY_TREE, homesOf } = await import('foundation-ui/settings/displayTree.js')
const { parseSettingsHash } = await import('foundation-ui/settings/index.js')

test('DT4 routes: an old address of 显示与视频 lands on the new place, by key and by place', () => {
  const to = (hash) => parseSettingsHash(hash, SEARCH_INDEX)
  assert.deepEqual(to('#tab=display&sub=video'), { tab: 'display', sub: 'advanced' }, 'the old 捕获与视频 is 高级')
  assert.deepEqual(to('#tab=av'), { tab: 'display', sub: 'advanced' })
  assert.deepEqual(to('#tab=advanced'), { tab: 'display', sub: 'advanced' })
  assert.deepEqual(to('#display'), { tab: 'display', sub: 'output' }, 'the first place')
  assert.deepEqual(to('#tab=display&sub=modes&focus=max_bitrate'), { tab: 'display', sub: 'encoders', focus: 'max_bitrate' }, 'the key decides')
  assert.deepEqual(to('#tab=display&sub=virtual&focus=vdd_headless_create'), { tab: 'display', sub: 'output', focus: 'vdd_headless_create' })
  assert.deepEqual(to('#tab=display&sub=encoders&focus=hdr_enhanced'), { tab: 'display', sub: 'hdr', focus: 'hdr_enhanced' })
  assert.deepEqual(to('#tab=display&sub=video&focus=adapter_name'), { tab: 'display', sub: 'advanced', focus: 'adapter_name' })
  assert.deepEqual(to('#nvenc_vdd_force_vsync'), { tab: 'display', sub: 'advanced', focus: 'nvenc_vdd_force_vsync' })
  assert.deepEqual(to('#mat_live'), { tab: 'display', sub: 'advanced', focus: 'mat_live' })
  assert.deepEqual(to('#tab=encoders&sub=nv&focus=nvenc_preset'), { tab: 'display', sub: 'encoders', focus: 'nvenc_preset' })
})

test('DT5 search: every key of the display pages that the index lists is in the tree, and every tree key is searchable', () => {
  const indexed = SEARCH_INDEX.filter((e) => e.params.tab === 'display' || e.id === 'mat_live').map((e) => e.id)
  for (const key of indexed) assert.equal(homesOf(key).length, 1, `${key} is in the index and has one home`)
  for (const p of DISPLAY_TREE) for (const s of p.sections) {
    const entries = SEARCH_INDEX.filter((e) => homesOf(e.id).some((h) => h.place === p.place && h.section === s.id))
    for (const e of entries) assert.equal(e.params.sub, p.place, `${e.id} is searched at its home`)
  }
})
