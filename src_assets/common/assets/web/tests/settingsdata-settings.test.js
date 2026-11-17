// SPDX-License-Identifier: GPL-3.0-only
// The checks of settings.test.js of foundation-ui that read this product's settings data (the search index and the label tables): they live here because the data does.
import assert from 'node:assert/strict'
import test from 'node:test'
import './helpers/foundationUi.mjs'
const { SEARCH_INDEX, LABELS } = await import('../config/settingsData/index.js')
const { ENCODER_IDS, buildSettingsEntries, dualsenseEntries, labelOf, parseSettingsHash, searchEntries, rowLines, settingsGroups, settingsHash } = await import('foundation-ui/settings/index.js')

const labelZh = (key) => labelOf('zh', key)

const labelEn = (key) => labelOf('en', key)

const entries = (label, alt) => buildSettingsEntries({ index: SEARCH_INDEX, label, altLabel: alt })

test('the list groups carry labels in both languages and a second line for every row', () => {
  const lines = rowLines({ locale: 'zh', config: { sunshine_name: 'Host', active_encoder: 'nvenc', port: 47989, upnp: 'disabled', hevc_mode: 0, av1_mode: 1 } })
  const items = settingsGroups({ label: labelZh, lines }).flatMap((g) => g.items)
  assert.equal(rowLines({ locale: 'zh', config: { active_encoder: 'nvenc', hevc_mode: 0, av1_mode: 1 } })['display-encoders'], 'NVENC · H.264/HEVC')
  assert.equal(items.find((i) => i.category === 'network').text, '端口 47989 · UPnP 关')
  assert.equal(items.find((i) => i.category === 'general').text, '语言、主机名称、睡眠模式', 'no live state that matters: what is inside')
  assert.ok(items.every((i) => i.text), 'no row is blank')
  assert.equal(items.find((i) => i.category === 'general').label, '常规')
  assert.equal(items.find((i) => i.category === 'appearance'), undefined, '个性化 has no row in the list')
})

test('a hash lands on tab, section and row; the key form and the old forms both work', () => {
  // the old address of a setting that moved from 音频/视频 into 显示 lands on it
  assert.deepEqual(parseSettingsHash('#tab=av&sub=display&focus=hdr_prep', SEARCH_INDEX), { tab: 'display', sub: 'hdr', focus: 'hdr_prep' })
  assert.deepEqual(parseSettingsHash('#focus=hevc_mode', SEARCH_INDEX), { tab: 'display', sub: 'encoders', focus: 'hevc_mode' })
  assert.deepEqual(parseSettingsHash('#network', SEARCH_INDEX), { tab: 'network' })
  assert.deepEqual(parseSettingsHash('#nv', SEARCH_INDEX), { tab: 'display', sub: 'encoders' })
  assert.deepEqual(parseSettingsHash('#bind_address', SEARCH_INDEX), { tab: 'network', focus: 'bind_address' }, 'the advanced settings of a tab are on the tab page')
  assert.equal(parseSettingsHash('#tab=nothing', SEARCH_INDEX), null)
  assert.equal(parseSettingsHash('', SEARCH_INDEX), null)
  assert.equal(settingsHash({ tab: 'display', sub: 'hdr', focus: 'x' }), '#tab=display&sub=hdr&focus=x')
  assert.ok(ENCODER_IDS.includes('nv'))
})

test('every setting of the index is found by its label in both languages and by its key name', () => {
  const zh = entries(labelZh, labelEn)
  const en = entries(labelEn, labelZh)
  assert.equal(zh.length, SEARCH_INDEX.length)
  for (const e of SEARCH_INDEX) {
    const inZh = zh.find((x) => x.keyName === e.id)
    const inEn = en.find((x) => x.keyName === e.id)
    // by key name (exact and with spaces), by Chinese label, by English label (also from the Chinese interface)
    assert.ok(searchEntries(zh, e.id, 8).includes(inZh), `key ${e.id}`)
    assert.ok(searchEntries(zh, e.id.replace(/_/g, ' '), 8).includes(inZh), `key words ${e.id}`)
    assert.ok(searchEntries(zh, inZh.title, 12).some((x) => x.keyName === e.id), `zh label ${e.id}`)
    assert.ok(searchEntries(en, inEn.title, 12).some((x) => x.keyName === e.id), `en label ${e.id}`)
    assert.ok(searchEntries(zh, inEn.title, 12).some((x) => x.keyName === e.id), `en label in zh ${e.id}`)
    assert.ok(inZh.path.length > 0)
  }
})

test('hevc_mode finds the codec row first and the result shows the category path', () => {
  const zh = entries(labelZh, labelEn)
  const [first] = searchEntries(zh, 'hevc_mode')
  assert.equal(first.keyName, 'hevc_mode')
  assert.equal(first.params.focus, 'hevc_mode')
  assert.ok(first.path.startsWith('显示与视频'))
  assert.equal(searchEntries(zh, 'hevc mode')[0].keyName, 'hevc_mode')
})

test('DualSense keys are searchable by label, key name and place', () => {
  const keys = [
    { id: 2, key: 'ds5.haptics.hd.gain', label: 'Haptics gain', group: 1, tier: 'common' },
    { id: 31, key: 'ds5.trigger.adaptive.slopeScale', label: 'Trigger adaptive slope Scale', group: 3, tier: 'advanced' },
  ]
  const ds = dualsenseEntries({ keys, label: labelZh, locale: 'zh', altLocale: 'en' })
  assert.equal(ds[0].params.sub, 'ds5-haptics')
  assert.equal(ds[1].params.sub, 'ds5-g3')
  assert.equal(ds[1].path, '输入 › 手柄 › 自适应扳机')
  assert.equal(searchEntries(ds, 'ds5.haptics.hd.gain')[0].keyName, 'ds5.haptics.hd.gain')
  assert.equal(searchEntries(ds, '触觉增益')[0].keyName, 'ds5.haptics.hd.gain')
  assert.equal(searchEntries(ds, 'slope')[0].keyName, 'ds5.trigger.adaptive.slopeScale')
  assert.equal(ds[0].route, 'config')
  assert.equal(ds[0].params.tab, 'input')
})

test('every language has the list texts or falls back to English', () => {
  for (const locale of Object.keys(LABELS)) assert.ok(labelOf(locale, 'tabs.general'), locale)
  assert.equal(labelOf('xx', 'tabs.general'), labelOf('en', 'tabs.general'))
  assert.equal(labelOf('zh', 'shell.recent'), '最近使用')
})
