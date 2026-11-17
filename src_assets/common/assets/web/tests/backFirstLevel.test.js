import test from 'node:test'
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import { DEFAULT_PLACE, PLACE_TABS, parentPlace } from 'foundation-ui/settings/index.js'

// A page reached from the rail or a row of its list is first level and has no Back; only a page inside a place (the gamepad's per-game page, a DualSense group) has one, to its place.
// Config.vue decides it; this is the same decision, read from the source and run over every place of every tab.
const config = await readFile(new URL('../views/Config.vue', import.meta.url), 'utf8')
const backOf = (tab, sub) => !!sub && !(!!PLACE_TABS[tab] && parentPlace(tab, sub) === sub)

test('Config.vue decides Back by the place: a place has none, a page inside a place has one', () => {
  assert.ok(config.includes('const canBack = computed(() => !!subNav.current.value && !(isPlace.value && parentPlace(tab.value, subNav.current.value) === subNav.current.value))'), 'canBack changed: run the back audit again (panel and Web, wide and single pane)')
  for (const [tab, places] of Object.entries(PLACE_TABS)) {
    for (const place of places) assert.equal(backOf(tab, place.id), false, `${tab}/${place.id} is a place of a list: no Back`)
    assert.equal(backOf(tab, ''), false, `${tab} without a place: the first place opens, no Back`)
  }
  assert.equal(backOf('input', 'gamepad-pergame'), true, 'a page inside 手柄 has Back')
  assert.equal(parentPlace('input', 'gamepad-pergame'), 'gamepad')
})

test('a page that belongs to no place never stays open: the first place opens instead of an empty page with a Back button', () => {
  assert.equal(parentPlace('display', 'keyboard'), '', 'a page of another tab belongs to no place of display')
  assert.equal(parentPlace('audio', 'nothing'), '')
  assert.ok(config.includes('PLACE_TABS[id] && !parentPlace(id, current)'), 'the guard reads the place of the page, not only an empty page')
  for (const tab of Object.keys(PLACE_TABS)) assert.ok(DEFAULT_PLACE[tab], `${tab} has a default place`)
})

test('inside the control panel frame the Web page tells the panel about a sub-page only through canBack', () => {
  assert.ok(config.includes("watch(canBack, (open) => { if (embedded) window.parent.postMessage({ type: 'foundation-subpage', open }, '*') }, { immediate: true })"))
})
