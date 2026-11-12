import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import { createClient, normalizeCatalog } from '../services/sparxieClient.js'
import { COPY } from '../utils/sparxie/copy.js'
import { fewStepChoice, gameDefaults, normalizeBlock, stableJson, gameFieldsOf, sparxieFieldsOf, splitUnavailable, withDefaults } from '../utils/sparxie/model.js'

// The host's own answers (written by tests/sparxie/dump_ui_fixtures.cpp, see sparxieSettingsRender.test.js) through the client and the model.
const fx = JSON.parse(readFileSync(new URL('./fixtures/sparxie-host.json', import.meta.url), 'utf8'))
const catalog = normalizeCatalog(fx.games.catalog)
const keys = (fields) => fields.map((f) => f.key)

test('the catalog the host sends describes nine titles: Zenless Zone Zero has the five file items first and only the input on the Sparxie side', () => {
  assert.deepEqual(Object.keys(catalog), ['genshin', 'starrail', 'zzz', 'hi3', 'wuwa', 'pgr', 'arknights', 'endfield', 'nte'])
  const zzz = keys(gameFieldsOf(catalog.zzz, 'zzz'))
  assert.deepEqual(zzz.slice(0, 7), ['resolution', 'display', 'fps', 'vsync', 'preset', 'renderScale', 'hdr'])
  for (const k of ['resolutionIndex', 'colorFilter', 'rayTracing', 'superRes', 'frameGen', 'masterVolume', 'textLanguage', 'gameBoost', 'customArgs']) assert.ok(zzz.includes(k), `Zenless Zone Zero describes ${k}`)
  assert.deepEqual(keys(sparxieFieldsOf(catalog.zzz, 'zzz')), ['input'])
  const rail = keys(gameFieldsOf(catalog.starrail, 'starrail'))
  assert.deepEqual(rail.slice(0, 6), ['resolution', 'display', 'fps', 'vsync', 'preset', 'renderScale'])
  assert.ok(!rail.includes('hdr'), 'Star Rail lists no HDR')
  assert.ok(keys(gameFieldsOf(catalog.genshin, 'genshin')).includes('hdr'))
  assert.deepEqual(keys(sparxieFieldsOf(catalog.genshin, 'genshin')), ['input', 'fpsUnlock', 'fps', 'bgLimit', 'bgFps', 'priority', 'followPreset', 'preset30', 'preset60', 'scaleOverride', 'scale'])
})

test('the items the host describes with `def` (not `default`) keep their defaults, text items and notes are read, and the 120 fps unlock is named and noted in both languages', () => {
  const by = Object.fromEntries(gameFieldsOf(catalog.starrail, 'starrail').map((f) => [f.key, f]))
  assert.equal(by.shadow.def, 'off'); assert.equal(by.masterVolume.def, 10); assert.equal(by.gameBoost.def, false)
  assert.equal(by.customArgs.kind, 'text', 'the launch arguments are a text item, not dropped')
  assert.ok(by.fps.note.includes('120') && by.fps.noteEn.includes('120'))
  assert.deepEqual(by.fps.options.map((o) => [o.value, o.label, o.labelEn]), [[30, '', ''], [60, '', ''], [120, '120（解锁）', '120 (unlock)']])
  assert.equal(by.preset.note, undefined)
})

test('Zenless Zone Zero values: frame rate 30 / 60 / unlimited, quality high / medium / low / custom, render scale 0.8 / 1.0 / 1.2 as a choice', () => {
  const by = Object.fromEntries(gameFieldsOf(catalog.zzz, 'zzz').map((f) => [f.key, f]))
  assert.deepEqual(by.fps.options.map((o) => o.value), [30, 60, 0])
  assert.deepEqual(by.preset.options.map((o) => o.value), ['high', 'medium', 'low', 'custom'])
  assert.equal(by.renderScale.kind, 'choice', 'three positions are a choice, not a track')
  assert.deepEqual(by.renderScale.options.map((o) => o.value), [0.8, 1, 1.2])
  assert.deepEqual(by.renderScale.options.map((o) => o.label), ['0.8', '1.0', '1.2'])
  assert.equal(by.vsync.kind, 'toggle'); assert.equal(by.hdr.kind, 'toggle')
  assert.equal(COPY.zh.fpsOpt(0), '不限')
  assert.deepEqual([COPY.zh.frameRate, COPY.zh.vsync, COPY.zh.renderScale, COPY.zh.preset, COPY.zh.hdr], ['游戏内帧率上限', '垂直同步', '渲染精度', '画质', 'HDR'])
  assert.deepEqual(['high', 'medium', 'low'].map((k) => COPY.zh.presetOpts[k]), ['高', '中', '低'])
  assert.equal(by.preset.options[3].label, '自定义', 'the host names the custom mode itself')
})

test('a slider with many positions stays a slider (Genshin render scale 0.6 to 1.5), the target frame rate of Sparxie is not the game\'s frame rate cap', () => {
  const scale = gameFieldsOf(catalog.genshin, 'genshin').find((f) => f.key === 'renderScale')
  assert.equal(scale.kind, 'slider')
  assert.deepEqual(fewStepChoice(scale), scale)
  assert.notEqual(COPY.zh.fpsCap, COPY.zh.frameRate)
  assert.notEqual(COPY.en.fpsCap, COPY.en.frameRate)
  assert.deepEqual(gameFieldsOf(catalog.genshin, 'genshin').find((f) => f.key === 'fps').options.map((o) => o.value), [30, 45, 60])
})

test('the game reports Zenless Zone Zero items it never saved as 60, V-sync on, 1.0, medium: so does a new preset', () => {
  const d = gameDefaults('zzz')
  assert.deepEqual([d.fps, d.vsync, d.renderScale, d.preset], [60, true, 1, 'medium'])
  assert.equal(gameDefaults('genshin').vsync, false)
  assert.deepEqual(withDefaults({ game: 'zzz' }).settings, {}, 'a new application holds nothing: its items follow the game')
  assert.equal(fx.gameSettings.zzzDefaults.values.vsync, true); assert.equal(fx.gameSettings.zzzDefaults.values.preset, 'medium')
})

test('the host\'s game-settings answer carries `unavailable`; the five items of an undecodable ZZZ file are split off, resolution and display stay', async () => {
  const client = createClient({ get: async () => fx.gameSettings.zzzUnreadable })
  const read = await client.gameSettings('zzz-cn')
  for (const k of ['fps', 'hdr', 'preset', 'renderScale', 'vsync', 'colorFilter', 'rayTracing']) assert.ok(read.unavailable.includes(k), `${k} cannot be read from a file the key does not decode`)
  assert.ok(!read.unavailable.includes('resolution') && !read.unavailable.includes('display'))
  const { shown, hidden } = splitUnavailable(gameFieldsOf(catalog.zzz, 'zzz'), read.unavailable)
  assert.deepEqual(keys(shown).slice(0, 2), ['resolution', 'display'])
  assert.deepEqual(keys(hidden).slice(0, 5), ['fps', 'vsync', 'preset', 'renderScale', 'hdr'])
  for (const k of ['fps', 'vsync', 'preset', 'renderScale', 'hdr']) assert.ok(!keys(shown).includes(k))
  const readable = await createClient({ get: async () => fx.gameSettings.zzz }).gameSettings('zzz-cn')
  assert.deepEqual(readable.unavailable, [])
  assert.equal(splitUnavailable(gameFieldsOf(catalog.zzz, 'zzz'), []).hidden.length, 0)
})

test('the read-only line names what cannot be changed and has no developer wording; a save refused for a running game has its own words', () => {
  const line = COPY.zh.gsUnsupported(['游戏内帧率上限', '垂直同步'].join(COPY.zh.listSep))
  assert.equal(line, '这个版本的游戏不支持修改：游戏内帧率上限、垂直同步')
  assert.ok(COPY.en.gsUnsupported('Frame rate cap, V-sync').startsWith('This game version'))
  for (const lang of ['zh', 'en']) for (const text of [COPY[lang].gsRunning, COPY[lang].gsRunningSave, COPY[lang].gsUnsupported('x')]) assert.ok(!/读不到|HTTP|API|file|文件/i.test(text), text)
  const create = readFileSync(new URL('../components/sparxie/SparxieNewApp.vue', import.meta.url), 'utf8')
  assert.equal(COPY.zh.errors.game_running, COPY.zh.gsRunningSave); assert.equal(COPY.en.errors.game_running, COPY.en.gsRunningSave)
  const settings = readFileSync(new URL('../components/sparxie/SparxieSettings.vue', import.meta.url), 'utf8')
  assert.ok(settings.includes('<MBanner v-if="unsupportedText"'))
})

test('a block the property-tree writer stringified gets its types back; a version that looks like a number stays text', () => {
  const stored = { artVersion: '12345678', launchTemplateProvisional: 'true', candidates: '', markers: ['exe'], settings: { fps: '60', vsync: 'false', renderScale: '1.2', hdr: 'false', resolution: '1920×1080', preset: 'medium' },
    options: { bgFps: '10', bgLimit: 'true', followPreset: 'false', fps: '120', fpsUnlock: 'false', input: 'kbm', isDefault: true, preset30: '60', preset60: '1000', priority: 'normal', scale: '400', scaleOverride: 'false' } }
  const b = normalizeBlock(stored)
  assert.deepEqual(b.settings, { fps: 60, vsync: false, renderScale: 1.2, hdr: false, resolution: '1920×1080', preset: 'medium' })
  assert.deepEqual(b.options, { bgFps: 10, bgLimit: true, followPreset: false, fps: 120, fpsUnlock: false, input: 'kbm', isDefault: true, preset30: 60, preset60: 1000, priority: 'normal', scale: 400, scaleOverride: false })
  assert.equal(b.artVersion, '12345678'); assert.equal(b.launchTemplateProvisional, true); assert.deepEqual(b.candidates, [])
  assert.equal(normalizeBlock({ options: { fps: 'display' } }).options.fps, 'display', 'the tier that follows the display is a word')
  assert.deepEqual(normalizeBlock({ settings: '' }).settings, {})
  assert.deepEqual(normalizeBlock(null), {})
  assert.deepEqual(normalizeBlock(b), b, 'a typed block is left alone')
})

test('opening an application and changing nothing is no change: the stringified block and the typed block are the same state', () => {
  const text = withDefaults({ game: 'zzz', settingsMode: 'preset', settings: { fps: '60', vsync: 'false' }, options: { fpsUnlock: 'false', fps: '120' } })
  const typed = withDefaults({ game: 'zzz', settingsMode: 'preset', settings: { fps: 60, vsync: false }, options: { fpsUnlock: false, fps: 120 } })
  assert.equal(stableJson(text), stableJson(typed))
  assert.equal(stableJson({ b: 1, a: [2, { d: 1, c: 2 }] }), stableJson({ a: [2, { c: 2, d: 1 }], b: 1 }), 'the order of the keys does not matter')
  const changed = withDefaults({ game: 'zzz', settingsMode: 'preset', settings: { fps: 30, vsync: false }, options: { fpsUnlock: false, fps: 120 } })
  assert.notEqual(stableJson(changed), stableJson(typed))
  const create = readFileSync(new URL('../components/sparxie/SparxieNewApp.vue', import.meta.url), 'utf8')
  assert.ok(create.includes(':disabled="!!missing || creating || unchanged"'), '保存 waits for a change')
  assert.ok(create.includes('baseline.value = state()'), 'and is off again after a save')
  assert.ok(/unchanged\.value\) return/.test(create), 'the save function itself refuses an unchanged page')
})
