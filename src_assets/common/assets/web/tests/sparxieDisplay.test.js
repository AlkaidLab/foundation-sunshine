import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { createRequire } from 'node:module'
import test, { after, before } from 'node:test'
import { createServer } from 'vite'
import vue from '@vitejs/plugin-vue'
import { foundationUiAliases } from '../../../../../scripts/vite/foundationUi.js'
import { COPY } from '../utils/sparxie/copy.js'
import { displayBody, effectiveLine, hasDisplay, normalizeDisplay, parseSize, resolutionChoices, startFixed, windowChoices } from '../utils/sparxie/display.js'
import { newBlock, withDefaults } from '../utils/sparxie/model.js'

// 显示: 跟随串流 (the default) or 固定, the choice of a game app. The shape the host stores (tests/sparxie/test_stream_display.cpp), the lists a fixed display offers,
// the one line that says what the display will be, and the section drawn with the real foundation-ui components.
const ZH = COPY.zh
const EN = COPY.en

test('an app that never chose follows the stream; only a fixed display carries values, and every value is text', () => {
  assert.deepEqual(normalizeDisplay(undefined), { mode: 'follow' })
  assert.deepEqual(normalizeDisplay({ mode: 'bogus', resolution: '1x1' }), { mode: 'follow' })
  assert.deepEqual(normalizeDisplay({ mode: 'follow', resolution: '1920×1080', window: 'windowed' }), { mode: 'follow' }, 'a follow display has no leftovers of a fixed one')
  assert.deepEqual(normalizeDisplay({ mode: 'fixed', resolution: '2560x1440', window: 'borderless' }), { mode: 'fixed', resolution: '2560×1440', window: 'borderless' })
  assert.deepEqual(normalizeDisplay({ mode: 'fixed', resolution: 'wide', window: 'exclusive', resolutionIndex: 'x' }), { mode: 'fixed', window: 'fullscreen' })
  assert.deepEqual(normalizeDisplay({ mode: 'fixed', window: 'fullscreen', resolutionIndex: '3' }), { mode: 'fixed', window: 'fullscreen', resolutionIndex: '3' })
  assert.deepEqual(displayBody({ mode: 'follow' }), { mode: 'follow' })
  assert.deepEqual(parseSize('2868 x 1320'), { width: 2868, height: 1320 })
  assert.equal(parseSize('wide'), null)
})

test('every application is given the default, new ones and the ones of the first builds alike', () => {
  assert.deepEqual(newBlock({ install: { game: 'starrail', id: 'starrail-cn' }, region: 'cn', mode: 'touch' }).display, { mode: 'follow' })
  const old = withDefaults({ game: 'starrail', region: 'cn', mode: 'touch', settingsMode: 'preset', settings: { resolution: '1920×1080', display: 'fullscreen' } })
  assert.deepEqual(old.display, { mode: 'follow' }, 'the 1920×1080 the first builds seeded was not a choice')
  assert.deepEqual(withDefaults({ game: 'endfield', display: { mode: 'fixed', resolution: '2560×1440', window: 'fullscreen' } }).display, { mode: 'fixed', resolution: '2560×1440', window: 'fullscreen' })
})

test('which titles have it, which window modes they offer, and what a fixed display starts from', () => {
  for (const g of ['genshin', 'starrail', 'zzz', 'hi3', 'wuwa', 'nte', 'endfield']) assert.ok(hasDisplay(g), g)
  for (const g of ['arknights', 'pgr', '']) assert.ok(!hasDisplay(g), g)
  for (const g of ['genshin', 'starrail', 'zzz', 'hi3', 'wuwa', 'nte', 'endfield']) assert.deepEqual(windowChoices(g), ['fullscreen', 'borderless', 'windowed'], `${g}: the three window modes`)
  assert.deepEqual(windowChoices('arknights'), [], 'a title without a 显示 section has none')
  assert.deepEqual(startFixed('starrail', { width: 2868, height: 1320 }), { mode: 'fixed', window: 'fullscreen', resolution: '2868×1320' })
  assert.deepEqual(startFixed('starrail', null), { mode: 'fixed', window: 'fullscreen', resolution: '1920×1080' })
  assert.deepEqual(startFixed('zzz', { width: 2560, height: 1440 }), { mode: 'fixed', window: 'fullscreen', resolutionIndex: '0' })
})

test('the sizes a fixed display lists: the usual ones, the display the host sees, and a stored size the list does not have', () => {
  const plain = resolutionChoices('starrail', null, '').map((r) => r.value)
  assert.deepEqual(plain, ['1280×720', '1600×900', '1920×1080', '2560×1440', '3840×2160'])
  assert.ok(resolutionChoices('starrail', { width: 2868, height: 1320 }, '').some((r) => r.value === '2868×1320'))
  const stored = resolutionChoices('starrail', null, '2400×1080').map((r) => r.value)
  assert.ok(stored.includes('2400×1080') && stored.indexOf('2400×1080') < stored.indexOf('1920×1080') + 2)
  const zzz = resolutionChoices('zzz', { width: 2560, height: 1440 })
  assert.equal(zzz[0].value, '0')
  assert.ok(zzz[0].native && zzz[0].mode === 'fullscreen')
  assert.deepEqual(resolutionChoices('zzz', null), [], 'a position in the game\'s list means nothing without the display')
})

test('the one line: the size when a stream runs and the host can read it, else that it follows the device; a fixed display says its size and mode', () => {
  assert.equal(effectiveLine({ mode: 'follow' }, 'starrail', { stream: { width: 2868, height: 1320 } }, ZH), '本次串流将以 2868×1320 全屏运行')
  assert.equal(effectiveLine({ mode: 'follow' }, 'starrail', {}, ZH), '随串流设备的分辨率全屏')
  assert.equal(effectiveLine({ mode: 'follow' }, 'starrail', { stream: null, screen: { width: 1920, height: 1080 } }, ZH), '随串流设备的分辨率全屏', 'the desktop of the host is not the stream')
  assert.equal(effectiveLine({ mode: 'fixed', resolution: '1920×1080', window: 'windowed' }, 'genshin', {}, ZH), '游戏将以 1920×1080 窗口运行')
  assert.equal(effectiveLine({ mode: 'fixed', resolution: '2560×1440' }, 'endfield', {}, ZH), '游戏将以 2560×1440 全屏运行')
  assert.equal(effectiveLine({ mode: 'fixed', resolution: '1920×1080', window: 'borderless' }, 'endfield', {}, ZH), `游戏将以 1920×1080 ${ZH.display.borderless}运行`)
  assert.equal(effectiveLine({ mode: 'fixed', window: 'fullscreen', resolutionIndex: '0' }, 'zzz', { screen: { width: 2560, height: 1440 } }, ZH), '游戏将以 2560×1440 全屏运行')
  assert.equal(effectiveLine({ mode: 'follow' }, 'starrail', { stream: { width: 2868, height: 1320 } }, EN), 'This stream runs fullscreen at 2868×1320')
  for (const C of [ZH, EN]) for (const text of [C.displayFollowLater, C.displayFollowNow(1, 2)]) assert.ok(text.length > 8 && !/你可以|这里|用于|就行|You can/.test(text), text)
})

// ---- the section, drawn ----
const foundationUi = process.env.FOUNDATION_UI_DIR || fileURLToPath(new URL('../../../../../../ds5-ui-m3e', import.meta.url))
const webRoot = fileURLToPath(new URL('..', import.meta.url))
let server
let render
before(async () => {
  server = await createServer({
    configFile: false, logLevel: 'silent', appType: 'custom', root: webRoot, server: { middlewareMode: true, hmr: false, watch: null }, optimizeDeps: { noDiscovery: true, include: [] },
    resolve: { dedupe: ['vue'], alias: foundationUiAliases(foundationUi) },
    ssr: { noExternal: ['foundation-ui', /^@material\//] },
    plugins: [vue({ template: { compilerOptions: { isCustomElement: (tag) => tag.startsWith('md-') } } })],
  })
  const require = createRequire(import.meta.url)
  const { createSSRApp, reactive } = require('vue')
  const { renderToString } = require('vue/server-renderer')
  const { createI18n } = require('vue-i18n')
  const Display = (await server.ssrLoadModule('/components/sparxie/SparxieDisplay.vue')).default
  render = async ({ game, display = { mode: 'follow' }, locale = 'zh' }) => {
    const block = reactive({ game, display: { ...display } })
    const i18n = createI18n({ legacy: false, locale, messages: { zh: {}, en: {} } })
    const vm = createSSRApp(Display, { block, installId: `${game}-cn`, region: 'cn' })
    vm.use(i18n)
    return renderToString(vm)
  }
})
after(async () => { await server?.close() })
const text = (html) => html.replace(/<[^>]+>/g, '\n').replace(/&amp;/g, '&').split('\n').map((s) => s.trim()).filter(Boolean)
const checked = (html, label) => new RegExp(`role="radio"[^>]*aria-checked="(true|false)"[^>]*aria-label="${label}"`).exec(html)?.[1]

test('a following display shows the two choices with 跟随串流 selected, the one line, and no size or window controls', async () => {
  const html = await render({ game: 'starrail' })
  const lines = text(html)
  assert.ok(lines.includes('显示'))
  assert.equal(checked(html, '跟随串流'), 'true')
  assert.equal(checked(html, '固定'), 'false')
  assert.ok(lines.includes('随串流设备的分辨率全屏'), lines.join(' | '))
  assert.ok(!lines.includes('分辨率') && !lines.includes('显示模式'), 'nothing to pick while following')
  const en = text(await render({ game: 'starrail', locale: 'en' }))
  assert.ok(en.includes('Follow the stream') && en.includes('Fixed') && en.includes('Fullscreen at the resolution of the streaming device'), en.join(' | '))
})

test('a fixed display offers the size and the window mode of its title, Endfield included; Zenless Zone Zero its list', async () => {
  const star = await render({ game: 'starrail', display: { mode: 'fixed', resolution: '1920×1080', window: 'windowed' } })
  assert.equal(checked(star, '固定'), 'true')
  const lines = text(star)
  assert.ok(lines.includes('分辨率') && lines.includes('显示模式'), lines.join(' | '))
  assert.ok(lines.includes('游戏将以 1920×1080 窗口运行'), lines.join(' | '))
  assert.ok(lines.includes('显示模式'), 'Unity titles: a window mode choice (全屏, 无边框窗口, 窗口)')
  const wuwa = text(await render({ game: 'wuwa', display: { mode: 'fixed', resolution: '1920×1080', window: 'fullscreen' } }))
  assert.ok(wuwa.includes('显示模式') && wuwa.includes('分辨率'), 'Unreal titles: a menu for the three window modes (a list of three long names is a menu, not segments)')
  const endfield = text(await render({ game: 'endfield', display: { mode: 'fixed', resolution: '2560×1440' } }))
  assert.ok(endfield.includes('分辨率') && endfield.includes('显示模式'), `Endfield: the size and the window mode like every Unity title: ${endfield.join(' | ')}`)
  const zzz = text(await render({ game: 'zzz', display: { mode: 'fixed', window: 'fullscreen', resolutionIndex: '0' } }))
  assert.ok(!zzz.includes('分辨率') && zzz.includes('显示模式'), 'without the display size a position in the list means nothing, so the window mode alone is offered')
})

test('a title the host does not size has no 显示 section', async () => {
  assert.equal(text(await render({ game: 'arknights' })).length, 0)
})

test('the page sends and compares the choice: it is part of the body and of the state, and the section sits above the game settings', () => {
  const create = readFileSync(new URL('../components/sparxie/SparxieNewApp.vue', import.meta.url), 'utf8')
  assert.ok(create.includes('display: displayBody(gameBlock.display)'), 'sent with a save and with a new app')
  assert.ok(create.includes('display: hasDisplay(gameBlock.game) ? displayState(gameBlock.display) : null'), 'part of what 保存 compares')
  assert.ok(create.indexOf('<SparxieDisplay') > 0 && create.indexOf('<SparxieDisplay') < create.indexOf('<SparxieSettings'))
  assert.ok(create.includes('Object.assign(gameBlock.display, normalizeDisplay(b.display))'), 'an edit opens with the stored choice, the default when it has none')
})
