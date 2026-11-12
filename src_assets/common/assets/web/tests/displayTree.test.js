import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'
import { DISPLAY_TREE, LEGACY_DISPLAY_KEYS, SEARCH_INDEX, homeOf } from 'foundation-ui/settings/index.js'
import { FORMATS, formatOf, hdrOf, modesFor } from '../composables/useCodecFormat.js'
import { gdiNameOf, SCALES } from '../composables/useDisplayLive.js'

const WEB = join(dirname(fileURLToPath(import.meta.url)), '..')
const read = (file) => readFileSync(join(WEB, file), 'utf8')

test('the page has one CfgSub for each place of the tree, in the files of the places', () => {
  const page = read('configs/tabs/Display.vue')
  const ids = [...page.matchAll(/<CfgSub[^>]*\bid="(\w+)"/g)].map((m) => m[1])
  assert.deepEqual(ids, DISPLAY_TREE.map((p) => p.place).filter((p) => p !== 'advanced').concat('advanced').sort((a, b) => ids.indexOf(a) - ids.indexOf(b)))
  assert.deepEqual([...ids].sort(), DISPLAY_TREE.map((p) => p.place).sort())
})

test('every setting the page draws is at the place of its home in the tree (the search index is generated from the page)', () => {
  const display = SEARCH_INDEX.filter((e) => e.params.tab === 'display' || e.id === 'mat_live')
  assert.ok(display.length >= Object.values(LEGACY_DISPLAY_KEYS).flat().length - 2, 'the page still has the settings of the old pages')
  for (const entry of display) assert.equal(homeOf(entry.id)?.place, entry.params.sub, `${entry.id} is drawn at ${entry.params.sub}, the tree puts it at ${homeOf(entry.id)?.place}`)
})

test('the old pages are gone: no template of the page still uses the lists and the pickers the tree replaced', () => {
  const all = ['Display.vue', 'display/DisplayOutput.vue', 'display/DisplayModes.vue', 'display/DisplayHdr.vue', 'display/DisplayEncoding.vue', 'display/DisplayVirtual.vue', 'display/DisplayAdvanced.vue'].map((f) => read(`configs/tabs/${f}`)).join('\n')
  for (const old of ['CodecStrategy', 'DisplayPreparationPicker', 'ModeListRows', 'VirtualDisplayPanel', 'CaptureVideo', 'ContainerEncoders', 'EncoderTiles']) assert.ok(!all.includes(old), `${old} is gone`)
  for (const used of ['MDisplayCards', 'MModeCard', 'MPrepSeg', 'MModePicker', 'MHdrStatus', 'MChoiceCards', 'MVddDevice', 'MExpander']) assert.ok(all.includes(used), `${used} is on the pages`)
  assert.ok(!/杂鱼|ZakoDisplay/.test(all))
})

test('the video format is the two codec modes seen as one choice: every pair of modes is one format, and a format writes a pair that reads back as itself', () => {
  assert.deepEqual(FORMATS, ['auto', 'h264', 'hevc', 'av1'])
  for (const hevc of [0, 1, 2, 3]) for (const av1 of [0, 1, 2, 3]) assert.ok(FORMATS.includes(formatOf({ hevc_mode: hevc, av1_mode: av1 })), `${hevc}/${av1}`)
  for (const format of FORMATS) for (const hdr of [false, true]) {
    const modes = modesFor(format, hdr)
    assert.equal(formatOf(modes), format, `${format} reads back`)
    assert.equal(hdrOf(modes), format === 'h264' ? false : hdr, `${format} keeps the HDR flag ${hdr}`)
  }
  assert.deepEqual(modesFor('auto', false), { hevc_mode: 0, av1_mode: 0 }, 'automatic with no HDR is the host default')
  assert.deepEqual(modesFor('h264', true), { hevc_mode: 1, av1_mode: 1 }, 'H.264 cannot carry HDR')
  assert.equal(formatOf({ hevc_mode: 2, av1_mode: 3 }), 'auto', 'both offered: the client chooses')
  assert.equal(formatOf({ hevc_mode: 3, av1_mode: 1 }), 'hevc')
})

test('the display mode card: the scaling stops are 100 to 300 in steps of 25 and the GDI name comes from the host data', () => {
  assert.equal(SCALES[0], 100)
  assert.equal(SCALES.at(-1), 300)
  assert.ok(SCALES.every((v, i) => i === 0 || v - SCALES[i - 1] === 25))
  assert.equal(gdiNameOf('DISPLAY NAME: \\\\.\\DISPLAY3\nFRIENDLY NAME: F32'), '\\\\.\\DISPLAY3')
  assert.equal(gdiNameOf('FRIENDLY NAME: F32'), '')
})

test('the HDR calibration and the display mode use the control panel commands that exist, and a browser gets no sliders (live = false)', () => {
  const live = read('composables/useDisplayLive.js')
  for (const command of ['list_display_modes', 'set_display_mode', 'get_current_dpi', 'set_desktop_dpi']) assert.ok(live.includes(`'${command}'`), command)
  assert.match(read('configs/tabs/display/DisplayHdr.vue'), /'launch_windows_hdr_calibration'/)
})
