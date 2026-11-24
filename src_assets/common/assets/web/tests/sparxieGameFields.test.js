import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import { normalizeCatalog } from '../services/sparxieClient.js'
import { COPY } from '../utils/sparxie/copy.js'
import { gameFieldsOf } from '../utils/sparxie/model.js'
import { controlOf, segmentsFit, customArgsProblem, firstProblem, isHidden, layoutGroups, outOfRange, zzzResolutionField, zzzResolutions } from '../utils/sparxie/gameFields.js'

// The layout of the game settings of the four miHoYo titles, from the host's own catalog (tests/sparxie/dump_ui_fixtures.cpp)
const fx = JSON.parse(readFileSync(new URL('./fixtures/sparxie-host.json', import.meta.url), 'utf8'))
const catalog = normalizeCatalog(fx.games.catalog)
const layout = (game, extra = {}) => layoutGroups(gameFieldsOf(catalog[game], game), { game, C: COPY.zh, screen: { width: 2560, height: 1440 }, ...extra })
const keysOf = (g) => g.cells.map((c) => c.field.key)

test('the groups come in the order of the game\'s own menu and each holds only what the host described for it', () => {
  for (const game of ['genshin', 'starrail', 'zzz', 'hi3']) {
    const groups = layout(game)
    assert.deepEqual(groups.map((g) => g.section), ['display', 'quality', 'audio', 'language', 'launch'].filter((s) => groups.some((g) => g.section === s)))
    assert.ok(groups.length >= 4, game)
    const all = groups.flatMap(keysOf)
    assert.equal(new Set(all).size, all.length, `${game}: no item twice`)
  }
})

test('items that do nothing yet or that another item replaces are not drawn', () => {
  assert.ok(isHidden('genshin', 'resizable') && isHidden('zzz', 'resizable'), 'no window hook in the host')
  for (const game of ['genshin', 'starrail', 'zzz', 'hi3']) {
    const all = layout(game).flatMap(keysOf)
    for (const k of ['resizable', 'launchWidth', 'launchHeight', 'launchFullscreen', 'popupWindow']) assert.ok(!all.includes(k), `${game} ${k}`)
  }
  assert.ok(layout('genshin').flatMap(keysOf).includes('dxVersion') && !layout('genshin').flatMap(keysOf).includes('graphicsApi'))
  assert.ok(layout('hi3').flatMap(keysOf).includes('graphicsApi') && !layout('hi3').flatMap(keysOf).includes('dxVersion'))
  // the size and the window mode are the app's 显示 choice: no second control for them in the grid of any title that has it
  for (const game of ['genshin', 'starrail', 'zzz', 'hi3', 'wuwa', 'nte', 'endfield']) {
    for (const k of ['resolution', 'display', 'resolutionIndex', 'exclusiveFullscreen', 'borderless', 'customResolution', 'customWidth', 'customHeight', 'launchWidth', 'launchHeight', 'launchFullscreen', 'popupWindow']) assert.ok(isHidden(game, k), `${game} ${k}`)
  }
  for (const game of ['wuwa', 'nte']) assert.ok(!layout(game).flatMap(keysOf).some((k) => ['resolution', 'display', 'launchWidth', 'popupWindow'].includes(k)), `${game}: the grid has none of them`)
  assert.ok(!isHidden('arknights', 'launchWidth') && !isHidden('pgr', 'resolution'), 'a title the host does not size keeps its items')
})

test('controls: short answers are segments, long lists are menus, volumes are sliders, pixel sizes are typed, the launch arguments are a field of their own', () => {
  const by = Object.fromEntries(layout('genshin').flatMap((g) => g.cells).map((c) => [c.field.key, c.control]))
  assert.equal(by.fps, 'segments'); assert.equal(by.fog, 'segments'); assert.equal(by.shadow, 'segments', 'four short names are segments'); assert.equal(by.environment, 'select', 'five are a menu'); assert.equal(by.textLanguage, 'select')
  assert.equal(by.masterVolume, 'slider'); assert.equal(by.renderScale, 'slider'); assert.equal(by.vsync, 'switch'); assert.equal(by.customArgs, 'args')
  assert.equal(controlOf({ kind: 'choice', options: [{ value: 'a', label: '部分队友特效' }, { value: 'b', label: '开' }, { value: 'c', label: '关' }] }), 'select', 'three long names do not fit as segments')
})

test('the group opens with what is set first: the frame rate (the size and the window mode are the 显示 choice); the graphics group with its sliders and the preset', () => {
  const z = layout('zzz')
  assert.equal(keysOf(z[0])[0], 'fps')
  const q = z.find((g) => g.section === 'quality')
  assert.equal(q.cells[0].control, 'slider')
  const g = layout('genshin').find((x) => x.section === 'quality')
  assert.ok(keysOf(g).indexOf('preset') < keysOf(g).indexOf('shadow'))
  // switches after menus, typed values last
  const rank = { select: 0, segments: 0, slider: 1, switch: 2, number: 3, args: 4 }
  const launch = layout('genshin').find((x) => x.section === 'launch')
  assert.deepEqual(launch.cells.map((c) => c.control), ['segments', 'switch', 'args'], 'the graphics API is three short names: segments')
  assert.ok(rank.switch < rank.args)
})

test('Zenless Zone Zero\'s resolution position becomes the list the game makes from the screen (Collapse ZenlessGameSettingsPage)', () => {
  const list = zzzResolutions({ width: 2560, height: 1440 })
  assert.deepEqual(list.slice(0, 3).map((r) => [r.index, r.mode, r.width, r.height]), [[0, 'fullscreen', 2560, 1440], [1, 'fullscreen', 2560, 1440], [2, 'fullscreen', 2276, 1280]])
  assert.ok(list.some((r) => r.mode === 'windowed' && r.width === 1920 && r.height === 1080))
  assert.equal(zzzResolutions({ width: 2520, height: 1080 }).filter((r) => r.mode === 'windowed')[0].width, 2520, 'an ultrawide screen lists its own size first among the windowed ones')
  assert.deepEqual(zzzResolutions(null), [])
  // the list is the 显示 section's (fixed display); the grid has no position item
  assert.ok(!layout('zzz').flatMap(keysOf).includes('resolutionIndex'))
  const f = zzzResolutionField(gameFieldsOf(catalog.zzz, 'zzz').find((x) => x.key === 'resolutionIndex'), { width: 2560, height: 1440 }, COPY.zh)
  assert.equal(f.normalize(-1), 0, 'the game stores -1 until it has been changed')
  assert.equal(f.normalize(3), 3)
  assert.ok(f.options[0].label.includes('原生'))
  assert.equal(zzzResolutionField({ key: 'resolutionIndex' }, null, COPY.zh), null, 'without the screen a position means nothing')
})

test('custom launch arguments: the rule of the host, with the line and the character', () => {
  assert.equal(customArgsProblem(''), null)
  assert.equal(customArgsProblem('-screen-quality 2\n"-force-d3d11"\r\n-a=b:c/d+e'), null)
  assert.deepEqual(customArgsProblem('-a\ncalc.exe & whoami'), { line: 2, ch: '&', kind: 'char' })
  assert.deepEqual(customArgsProblem('"-a b"'), { line: 1, ch: ' ', kind: 'char' }, 'a space inside quotes is refused as the host does')
  assert.deepEqual(customArgsProblem('"-a'), { line: 1, ch: '"', kind: 'quote' })
  for (const bad of ['a|b', 'a;b', 'a>b', '$x', '`x`', '%PATH%', 'a^b']) assert.ok(customArgsProblem(bad), bad)
  assert.ok(COPY.zh.argsChar(2, '&').includes('可能执行其他命令') && COPY.en.argsChar(2, '&').includes('another command'), 'the page says why')
})

test('what stops a save: a number outside its range, arguments the host would refuse; an item that is off is not checked', () => {
  const groups = layout('genshin')
  const values = (over) => (key, f) => (key in over ? over[key] : f ? f.def : undefined)
  assert.equal(firstProblem(groups, values({})), null)
  assert.deepEqual(firstProblem(groups, values({ customArgs: 'a & b' })), { key: 'customArgs', kind: 'args' })
  assert.ok(outOfRange({ min: 640, max: 7680 }, 'x') && !outOfRange({ min: 640, max: 7680 }, 1920))
})

test('the editor draws no filler: every new line carries a state, a consequence or a requirement', () => {
  for (const lang of ['zh', 'en']) {
    const C = COPY[lang]
    for (const text of [C.hints.gameBoost, C.argsHint, C.argsQuote(1), C.settingsProblem('Launch arguments')]) assert.ok(text.length > 8 && !/你可以|这里|用于|就行/.test(text), text)
  }
})

test('one rule for every choice of every game: up to four short names are segments, whichever page or title the item is on', () => {
  assert.ok(segmentsFit(['自动', '触控', '键鼠与手柄']), 'the input of Endfield')
  assert.ok(segmentsFit(['触控', '键鼠与手柄']) && segmentsFit(['低于正常', '正常', '高于正常', '高']), 'the input and the process priority, as in every other title')
  assert.ok(segmentsFit(['最低', '低', '中', '高']) && segmentsFit(['自动', 'DirectX 11', 'DirectX 12']), 'a four-step quality item and the graphics API')
  assert.ok(!segmentsFit(['a', 'b', 'c', 'd', 'e']), 'five answers are a menu')
  assert.ok(!segmentsFit(['Touch', 'Keyboard, mouse and gamepad']), 'names too long for one row are a menu')
  // every choice of every title in the full catalog: the control follows the rule and nothing else
  for (const row of Object.values(catalog)) for (const f of gameFieldsOf(row, row.game).filter((x) => x.kind === 'choice')) {
    const names = f.options.map((o) => o.label || o.labelEn || String(o.value))
    assert.equal(controlOf(f, true), segmentsFit(names) ? 'segments' : 'select', `${row.game}.${f.key}`)
  }
})
