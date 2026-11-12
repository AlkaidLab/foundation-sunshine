import './helpers/foundationUi.mjs'
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'

const { SEARCH_INDEX, buildSettingsEntries, categoryOfRow, dualsenseEntries, ds5PlaceOf, labelOf, searchEntries, settingsGroups } = await import('foundation-ui/settings/index.js')

// The shell keeps its pages and rail items in two literals; read them from the source so the test sees what the shell has.
const shellSource = await readFile(new URL('../views/WebShell.vue', import.meta.url), 'utf8')
const literal = (name) => {
  const start = shellSource.indexOf(`const ${name} = `)
  assert.ok(start >= 0, `${name} is in the shell`)
  const open = shellSource.indexOf(shellSource[shellSource.indexOf('=', start) + 2], start)
  const close = shellSource.indexOf('\n]', open) >= 0 && shellSource[open] === '[' ? shellSource.indexOf('\n]', open) + 2 : shellSource.indexOf('\n}', open) + 2
  return new Function('aboutTexts', `return (${shellSource.slice(open, close)})`)(() => ({ title: 'About' }))
}
const PAGES = literal('PAGES')
const RAIL = literal('RAIL')
const zh = (key) => labelOf('zh', key)
const en = (key) => labelOf('en', key)

test('DualSense is neither a rail item, a row of any list pane, nor a page of the Web UI', () => {
  assert.ok(RAIL.length >= 4)
  assert.ok(!RAIL.some((r) => /dualsense/i.test(`${r.id} ${r.label}`)))
  const lists = RAIL.map((r) => ({
    rail: r.id,
    ids: r.id === 'config'
      ? settingsGroups({ label: zh }).flatMap((g) => g.items.map((i) => i.id))
      : Object.entries(PAGES).filter(([, p]) => p.rail === r.id).map(([id]) => id),
  }))
  for (const l of lists) assert.ok(!l.ids.some((id) => /dualsense/i.test(id)), `rail ${l.rail}: ${l.ids}`)
  assert.equal(PAGES.dualsense, undefined, 'no page of its own')
  assert.ok(!SEARCH_INDEX.some((e) => /dualsense/.test(e.id)))
})

test('the old address /dualsense opens the gamepad page of 输入', () => {
  assert.match(shellSource, /normalise\(location\.pathname\) === '\/dualsense'\) return \[\{ id: 'gamepad' \}\]/)
  assert.ok(!/import\('\.\/DualSense\.vue'\)/.test(shellSource))
})

test('the gamepad page holds the DualSense block under the type cards, and the cards are its sub-pages', async () => {
  const inputs = await readFile(new URL('../configs/tabs/Inputs.vue', import.meta.url), 'utf8')
  assert.ok(inputs.indexOf('<CfgCards') < inputs.indexOf('<DualSenseGamepad'), 'the block sits directly under the type cards')
  assert.ok(!/dualsense_page|openDualSense|nav\.go\('dualsense'/.test(inputs), 'no row opens a page of its own')
  assert.match(inputs, /<DualSenseSubs/)
  // an unset type is the host's default (automatic): the block shows for it too
  assert.match(inputs, /dualsenseShown = computed\(\(\) => props\.platform !== 'macos' && \(\(config\.value\.gamepad \?\? 'auto'\) === 'ds5' \|\| \(config\.value\.gamepad \?\? 'auto'\) === 'auto'\)\)/)
  // DS4 wording is not on the page of a DualSense: the emulation switches are at the bottom of the gamepad page, and only for "automatic"
  const gamepadPage = inputs.slice(inputs.indexOf('<CfgSub id="gamepad"'), inputs.indexOf('<CfgSub id="keyboard"'))
  const at = (needle) => gamepadPage.indexOf(needle)
  assert.ok(at('k="motion_as_ds4"') > at('<DualSenseGamepad') && at('k="touchpad_as_ds4"') > at('<DualSenseGamepad'))
  assert.match(gamepadPage, /\(config\.gamepad \?\? 'auto'\) === 'auto' && platform === 'windows'">\s*<CfgSwitch :config="config" k="motion_as_ds4"/)
  const block = await readFile(new URL('../components/dualsense/DualSenseGamepad.vue', import.meta.url), 'utf8')
  assert.match(block, /nav\.open\(DS5_SUB\.card\(card\.id\)\)/)
  assert.match(block, /nav\.open\(DS5_SUB\.group\(entry\.id\)\)/)
  assert.ok(!/MPage|backFromPage/.test(block), 'the block has no page frame and no back target of its own: the sub-page bubble returns to the gamepad page')
})

test('every one of the 106 keys is reachable from the gamepad page', async () => {
  const keys = JSON.parse(await readFile(new URL('./fixtures/dualsense-keys.json', import.meta.url), 'utf8'))
  if (!keys.length) return
  assert.equal(keys.length, 106)
  for (const key of keys) {
    const place = ds5PlaceOf(key)
    assert.ok(place.sub || key.id === 1, `${key.key} has a place`)
  }
})

test('search shows 输入 › 手柄 › card and opens the sub-page of the gamepad page', () => {
  const rows = dualsenseEntries({ keys: [{ id: 3, key: 'ds5.haptics.hd.gain', label: 'Haptics gain', group: 1, tier: 'common' }], label: zh, locale: 'zh' })
  assert.equal(rows[0].path, '输入 › 手柄 › 触觉与震动')
  assert.equal(rows[0].route, 'config')
  assert.deepEqual(rows[0].params, { tab: 'input', sub: 'ds5-haptics', focus: 'ds5:ds5.haptics.hd.gain' })
  assert.equal(categoryOfRow(rows[0].route, rows[0].params), 'input')
  assert.ok(buildSettingsEntries({ index: SEARCH_INDEX, label: en }).every((e) => !/DualSense page/i.test(e.title)))
  assert.equal(searchEntries(buildSettingsEntries({ index: SEARCH_INDEX, label: zh, altLabel: en }), 'gamepad', 3).length > 0, true)
})
