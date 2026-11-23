// SPDX-License-Identifier: GPL-3.0-only
// The checks of inputplaces.test.js of foundation-ui that read this product's settings data (the search index and the label tables): they live here because the data does.
import assert from 'node:assert/strict'
import test from 'node:test'
import './helpers/foundationUi.mjs'
const { SEARCH_INDEX, LABELS } = await import('../config/settingsData/index.js')
const { DEFAULT_PLACE, INPUT_PLACES, PLACE_TABS, SETTINGS_GROUPS, labelOf, parseSettingsHash, placesOf, searchEntries, buildSettingsEntries, settingsHash } = await import('foundation-ui/settings/index.js')

const OLD_INPUT_KEYS = ['controller', 'gamepad', 'ds4_back_as_touchpad_click', 'usb_forwarding_enabled', 'back_button_timeout', 'keyboard', 'always_send_scancodes', 'key_rightalt_to_key_win', 'mouse',
  'high_resolution_scrolling', 'capture_cursor', 'native_pen_touch', 'usb_forwarding_port', 'motion_as_ds4', 'touchpad_as_ds4', 'enable_dsu_server', 'dsu_server_port', 'key_repeat_delay',
  'key_repeat_frequency', 'native_touchpad_optimization', 'amf_draw_mouse_cursor', 'virtual_mouse']

const WHERE = {
  gamepad: ['controller', 'gamepad', 'ds4_back_as_touchpad_click', 'back_button_timeout', 'motion_as_ds4', 'touchpad_as_ds4', 'enable_dsu_server', 'dsu_server_port'],
  keyboard: ['keyboard', 'always_send_scancodes', 'key_rightalt_to_key_win', 'key_repeat_delay', 'key_repeat_frequency'],
  mouse: ['mouse', 'high_resolution_scrolling', 'capture_cursor', 'native_pen_touch', 'native_touchpad_optimization', 'amf_draw_mouse_cursor', 'virtual_mouse'],
  usb: ['usb_forwarding_enabled', 'usb_forwarding_port'],
}
const INPUT_PAGES = ['gamepad', 'keyboard', 'mouse', 'usb']

const entry = (id) => SEARCH_INDEX.filter((e) => e.id === id)

test('every key of the old input page is on exactly one of the four pages, and each page has what was named for it', () => {
  for (const key of OLD_INPUT_KEYS) assert.equal(entry(key).length, 1, `${key} is in the index once`)
  const placed = OLD_INPUT_KEYS.map((key) => entry(key)[0].params)
  for (const [key, p] of OLD_INPUT_KEYS.map((k, i) => [k, placed[i]])) assert.equal(p.tab, 'input', `${key} stays in 输入与外设`)
  for (const [place, keys] of Object.entries(WHERE)) for (const key of keys) assert.equal(entry(key)[0].params.sub, place, `${key} is on ${place}`)
  assert.equal(Object.values(WHERE).flat().length, OLD_INPUT_KEYS.length, 'no key is on two pages')
  assert.equal(entry('clipboard_sync')[0].params.sub, 'keyboard')
  assert.deepEqual(SEARCH_INDEX.filter((e) => e.params.tab === 'input' && !INPUT_PAGES.includes(e.params.sub)), [], 'no entry is on a page outside the four rail places (no 高级, no drivers page)')
})

test('the places of 输入与外设: 手柄, 键盘, 鼠标与触控, USB 转发 (Windows only); the first one is the default; the rows are not under the stream settings', () => {
  assert.deepEqual(INPUT_PLACES.map((p) => p.id), ['gamepad', 'keyboard', 'mouse', 'usb'])
  assert.equal(PLACE_TABS.input, INPUT_PLACES); assert.equal(DEFAULT_PLACE.input, 'gamepad')
  assert.deepEqual(placesOf('input', 'linux').map((p) => p.id), ['gamepad', 'keyboard', 'mouse']); assert.equal(placesOf('input', 'windows').length, 4)
  assert.equal(SETTINGS_GROUPS.flatMap((g) => g.categories).includes('input'), false)
  assert.equal(labelOf('zh', 'config.input_mouse_title'), '鼠标与触控'); assert.equal(labelOf('zh', 'shell.usb_forwarding'), 'USB 转发')
})

test('the old addresses of 输入 lead to the page of the key, else to 手柄', () => {
  const at = (hash) => parseSettingsHash(hash, SEARCH_INDEX)
  assert.deepEqual(at('#tab=input'), { tab: 'input', sub: 'gamepad' })
  assert.deepEqual(at('#tab=input&focus=ds5-block'), { tab: 'input', sub: 'gamepad', focus: 'ds5-block' })
  assert.deepEqual(at('#tab=input&focus=always_send_scancodes'), { tab: 'input', sub: 'keyboard', focus: 'always_send_scancodes' })
  assert.deepEqual(at('#tab=input&sub=expert&focus=key_repeat_delay'), { tab: 'input', sub: 'keyboard', focus: 'key_repeat_delay' })
  assert.deepEqual(at('#tab=input&sub=drivers&focus=virtual_mouse'), { tab: 'input', sub: 'mouse', focus: 'virtual_mouse' })
  assert.deepEqual(at('#tab=input&sub=drivers'), { tab: 'input', sub: 'gamepad' })
  assert.deepEqual(at('#tab=input&sub=expert'), { tab: 'input', sub: 'gamepad' })
  assert.deepEqual(at('#input'), { tab: 'input', sub: 'gamepad' })
  assert.deepEqual(at('#usb_forwarding_port'), { tab: 'input', sub: 'usb', focus: 'usb_forwarding_port' })
  assert.deepEqual(at('#tab=input&sub=gamepad-pergame'), { tab: 'input', sub: 'gamepad-pergame' }, 'a page inside the gamepad page keeps its address')
  assert.equal(settingsHash({ tab: 'input', sub: 'keyboard', focus: 'keyboard' }), '#tab=input&sub=keyboard&focus=keyboard')
})

test('the search lands on the right page, with the rail in its path', () => {
  const rows = buildSettingsEntries({ index: SEARCH_INDEX, label: (k) => labelOf('zh', k) })
  const hit = (q) => searchEntries(rows, q)[0]
  assert.deepEqual([hit('key_repeat_delay').params.tab, hit('key_repeat_delay').params.sub], ['input', 'keyboard'])
  const repeat = hit('按键重复')
  assert.ok(repeat, 'the Chinese label finds the setting')
  assert.equal(repeat.params.sub, 'keyboard')
  assert.equal(hit('virtual_mouse').params.sub, 'mouse'); assert.equal(hit('dsu_server_port').params.sub, 'gamepad'); assert.equal(hit('usb_forwarding_enabled').params.sub, 'usb')
  assert.equal(hit('capture_cursor').path, '输入与外设 › 鼠标与触控')
})
