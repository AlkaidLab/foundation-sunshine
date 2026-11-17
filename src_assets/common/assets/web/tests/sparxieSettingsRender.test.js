import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { createRequire } from 'node:module'
import test, { after, before } from 'node:test'
import { createServer } from 'vite'
import vue from '@vitejs/plugin-vue'
import { foundationUiAliases } from '../../../../../scripts/vite/foundationUi.js'
import { normalizeCatalog } from '../services/sparxieClient.js'
import { COPY } from '../utils/sparxie/copy.js'
import { withDefaults } from '../utils/sparxie/model.js'

// The game settings of an application drawn from the host's OWN answers: tests/fixtures/sparxie-host.json is written by the host's sparxie_ui_fixtures tool
// (tests/sparxie/dump_ui_fixtures.cpp: catalog_json(), game-settings reads of a registry / GENERAL_DATA.bin, the applications the host stores), not by hand.
// SparxieSettings is server-rendered here with the real foundation-ui components; what a person sees is its text and the controls it draws.
const fx = JSON.parse(readFileSync(new URL('./fixtures/sparxie-host.json', import.meta.url), 'utf8'))
const catalog = normalizeCatalog(fx.games.catalog)
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
  const Settings = (await server.ssrLoadModule('/components/sparxie/SparxieSettings.vue')).default
  render = async ({ app, game, read, locale = 'zh', onProblem, fpsBySparxie = false }) => {
    const block = reactive({ game, restore: 'restore', settings: { ...app.settings } })
    const i18n = createI18n({ legacy: false, locale, messages: { zh: {}, en: {} } })
    const vm = createSSRApp(Settings, { block, row: catalog[game], installId: app.install, region: app.region, fpsBySparxie, initial: read, ...(onProblem ? { onProblem } : {}) })
    vm.use(i18n)
    return renderToString(vm)
  }
})
after(async () => { await server?.close() })

const text = (html) => html.replace(/<[^>]+>/g, '\n').replace(/&amp;/g, '&').split('\n').map((s) => s.trim()).filter((s) => s && s !== COPY.zh.gsFollow && s !== COPY.zh.gsChanged && s !== COPY.en.gsFollow && s !== COPY.en.gsChanged) // the marks of an item sit between its name and its value
const appOf = (game) => fx.apps.find((a) => a.sparxie.game === game && a.sparxie.mode === 'touch').sparxie

const checked = (html, kind, label) => new RegExp(`role="${kind}"[^>]*aria-checked="(true|false)"[^>]*aria-label="${label}"`).exec(html)?.[1]
const appOfAny = (game) => fx.apps.find((a) => a.sparxie.game === game).sparxie

test('Zenless Zone Zero: the five file items are drawn with their Chinese names, three-position items as segments and the rest as menus', async () => {
  const html = await render({ app: appOf('zzz'), game: 'zzz', read: fx.gameSettings.zzz })
  const lines = text(html)
  for (const n of ['游戏内帧率上限', '垂直同步', '画质', '渲染精度', 'HDR']) assert.ok(lines.includes(n), `${n}: ${lines.join(' | ')}`)
  for (const word of ['30', '60', '不限', '0.8', '1.0', '1.2']) assert.ok(lines.includes(word), `segment ${word}`)
  assert.equal(checked(html, 'radio', '60'), 'true', 'the 60 fps segment is selected')
  assert.ok(!lines.some((l) => /读不到|无法读取|HTTP|接口|字段/.test(l)), 'no developer wording')
  assert.ok(!lines.some((l) => l.startsWith('这个版本的游戏不支持修改')), 'a readable file has no read-only line')
})

test('Zenless Zone Zero: the render scale (0.8, 1.0, 1.2) is segments, not a slider track', async () => {
  const html = await render({ app: appOf('zzz'), game: 'zzz', read: fx.gameSettings.zzz })
  assert.ok(/class="gs-seg[^"]*"[^>]*><span class="gs-lab-row"><span class="gs-lab">渲染精度/.test(html), 'segments under the name')
  assert.ok(!/gs-slide[^>]*><span class="gs-slide__head"><span class="gs-lab-row"><span class="gs-lab">渲染精度/.test(html), 'no slider for it')
})

test('Zenless Zone Zero, file not decodable: the file items are not drawn, one short line says so', async () => {
  const lines = text(await render({ app: appOf('zzz'), game: 'zzz', read: fx.gameSettings.zzzUnreadable }))
  const line = lines.find((l) => l.startsWith('这个版本的游戏不支持修改：'))
  assert.ok(line && line.startsWith('这个版本的游戏不支持修改：游戏内帧率上限、垂直同步、画质、渲染精度、HDR'), lines.join(' | '))
  for (const n of ['垂直同步', '渲染精度', 'HDR', '画质']) assert.ok(!lines.includes(n), `${n} is not drawn as an item`)
  for (const n of ['分辨率', '显示模式']) assert.ok(!lines.includes(n), `${n} is the 显示 choice's, not an item of the grid`)
})

test('a file that was never written reads as the game\'s own defaults: 60, V-sync on, 1.0, medium: they are what the preset holds, so nothing is said to differ', async () => {
  const html = await render({ app: { ...appOf('zzz'), settings: {} }, game: 'zzz', read: fx.gameSettings.zzzDefaults })
  assert.equal(checked(html, 'radio', '60'), 'true')
  assert.equal(checked(html, 'radio', '1.0'), 'true')
  assert.equal(checked(html, 'switch', '垂直同步'), 'true')
  assert.ok(!text(html).some((l) => l.startsWith('当前')), 'the game has what the preset holds')
})

test('where the preset differs from the game the small line says what the game has', async () => {
  const lines = text(await render({ app: { ...appOf('zzz'), settings: { vsync: false, renderScale: 1.2 } }, game: 'zzz', read: fx.gameSettings.zzzDefaults }))
  assert.ok(lines.includes('当前 开') && lines.includes('当前 1.0'), lines.join(' | '))
})

test('Genshin: the 45 fps tier is a segment, HDR is a switch; Star Rail has no HDR', async () => {
  const g = text(await render({ app: appOf('genshin'), game: 'genshin', read: fx.gameSettings.genshin }))
  for (const w of ['30', '45', '60', 'HDR']) assert.ok(g.includes(w), w)
  const s = text(await render({ app: appOf('starrail'), game: 'starrail', read: fx.gameSettings.starrail }))
  assert.ok(!s.includes('HDR'), 'Star Rail lists no HDR')
  assert.ok(s.includes('垂直同步') && s.includes('画质'))
})

test('the groups are the five of the game\'s own menu, in order, each a card with its title', async () => {
  const lines = text(await render({ app: appOf('genshin'), game: 'genshin', read: fx.gameSettings.genshin }))
  const at = ['显示', '图像', '声音', '语言', '启动'].map((n) => lines.indexOf(n))
  assert.ok(at.every((i) => i >= 0), lines.join(' | '))
  assert.deepEqual([...at].sort((x, y) => x - y), at)
  const html = await render({ app: appOf('genshin'), game: 'genshin', read: fx.gameSettings.genshin })
  assert.equal((html.match(/<section class="gs-card"/g) || []).length, 5)
})

test('volumes are sliders with their value beside the name, the languages are menus', async () => {
  const html = await render({ app: appOf('genshin'), game: 'genshin', read: fx.gameSettings.genshin })
  assert.equal((html.match(/class="gs-slide\b/g) || []).length, 5, 'four volumes and the render scale')
  for (const n of ['总音量', '音乐音量', '音效音量', '语音音量']) assert.ok(new RegExp(`<span class="gs-lab">${n}</span>[\\s\\S]*?</span><span class="gs-slide__val">10</span>`).test(html), n)
  assert.ok(/class="gs-seg[^"]*"[^>]*><span class="gs-lab-row"><span class="gs-lab">语音<\/span>/.test(html), 'the audio language: four short names are segments, like every other item by the one rule')
  assert.ok(/<span id="[^"]*" class="gs-lab">文本<\/span>[\s\S]*?<button[^>]*class="gs-sel__btn"/.test(html), 'the text language, a long list, is a menu')
  const hi = await render({ app: appOfAny('hi3'), game: 'hi3', read: fx.gameSettings.hi3 })
  assert.ok(/<span class="gs-lab">总音量<\/span>[\s\S]*?<\/span><span class="gs-slide__val">80<\/span>/.test(hi) && /aria-valuemax="100"/.test(hi), 'Honkai Impact 3rd volumes go to 100')
})

test('Star Rail: 120 is an unlock (30 / 60 / 120（解锁）) with its sentence right under the item, in the language of the page', async () => {
  const html = await render({ app: appOf('starrail'), game: 'starrail', read: fx.gameSettings.starrail })
  const zh = text(html)
  assert.ok(zh.includes('游戏内帧率上限') && zh.includes('120（解锁）'), zh.join(' | '))
  assert.ok(/class="gs-seg[^"]*"[^>]*><span class="gs-lab-row"><span class="gs-lab">游戏内帧率上限<\/span>.*?<small class="gs-hint">120 由 Sunshine 写入；之后在游戏内改帧率会掉回去，请在这里改。/.test(html.replace(/\n/g, ' ')), 'the sentence is in the cell of the frame rate, not under the grid')
  assert.ok(!/class="gs-note"/.test(html), 'no line under the grid')
  const en = text(await render({ app: appOf('starrail'), game: 'starrail', read: fx.gameSettings.starrail, locale: 'en' }))
  assert.ok(en.includes('120 is written by Sunshine. Changing the in-game frame rate later drops it back; change it here.'), en.join(' | '))
  // Sparxie's unlock takes the item over as before
  const locked = text(await render({ app: appOf('starrail'), game: 'starrail', read: fx.gameSettings.starrail, fpsBySparxie: true }))
  assert.ok(locked.includes('帧率由 Sparxie 接管') && !locked.includes('120（解锁）'))
  // other titles carry no such sentence
  assert.ok(!text(await render({ app: appOf('genshin'), game: 'genshin', read: fx.gameSettings.genshin })).some((l) => l.includes('Sunshine 写入')))
})

test('what the page does not draw: the resizable window (no window hook in the host), the older launch items that the described ones replace, the API item that is not real for the title', async () => {
  const draw = async (game) => text(await render({ app: appOfAny(game), game, read: fx.gameSettings[game] }))
  for (const game of ['genshin', 'starrail', 'zzz', 'hi3']) {
    const html = await render({ app: appOfAny(game), game, read: fx.gameSettings[game] })
    const lines = await draw(game)
    for (const gone of ['可调整大小的窗口', '启动宽度', '启动高度', '启动时全屏', '无边框窗口参数']) assert.ok(!lines.includes(gone), `${game} does not draw ${gone}`)
    assert.equal(lines.filter((l) => l === '图形 API').length, 1, `${game} has one 图形 API`)
    assert.ok(lines.includes('提升游戏优先级') && html.includes('label="自定义启动参数"'), `${game} has the priority switch and the launch arguments`)
  }
})

test('提升游戏优先级 says what it does; 自定义启动参数 says what it allows', async () => {
  const lines = text(await render({ app: appOf('genshin'), game: 'genshin', read: fx.gameSettings.genshin }))
  assert.ok(lines.includes('游戏启动后，进程优先级调为“高于正常”'))
  const html = await render({ app: appOf('genshin'), game: 'genshin', read: fx.gameSettings.genshin })
  assert.ok(html.includes('supporting-text="每行可写多个参数。只允许字母、数字和 _ . / : = + -'))
})

test('a launch argument the host would refuse is told with the character and the line, and the page reports it', async () => {
  const seen = []
  const html = await render({ app: { ...appOf('genshin'), settings: { customArgs: '-screen-quality 2\ncalc.exe & whoami' } }, game: 'genshin', read: fx.gameSettings.genshin, onProblem: (t) => seen.push(t) })
  assert.ok(html.includes('error-text="第 2 行的 “&amp;” 不能使用：它在命令行里有特殊含义，可能执行其他命令"'), html.slice(html.indexOf('error-text') - 50, html.indexOf('error-text') + 200))
  assert.deepEqual(seen, ['游戏设置有误：自定义启动参数'])
  const fine = []
  await render({ app: { ...appOf('genshin'), settings: { customArgs: '-screen-quality 2\n-force-d3d11' } }, game: 'genshin', read: fx.gameSettings.genshin, onProblem: (t) => fine.push(t) })
  assert.deepEqual(fine, [''])
})

test('Zenless Zone Zero: the resolution position is not an item of the grid (it is the 显示 section\'s list)', async () => {
  const lines = text(await render({ app: appOf('zzz'), game: 'zzz', read: fx.gameSettings.zzz }))
  assert.ok(!lines.includes('游戏内分辨率') && !lines.includes('分辨率'))
})

test('the frame rate that Sparxie unlocks is shown off with that line', async () => {
  const lines = text(await render({ app: appOf('genshin'), game: 'genshin', read: fx.gameSettings.genshin, fpsBySparxie: true }))
  assert.ok(lines.includes('帧率由 Sparxie 接管'))
})

test('a running game: one short line says the changes apply from the next launch, and the controls stay usable; nothing is said while the game is closed', async () => {
  const running = await render({ app: appOf('zzz'), game: 'zzz', read: { ...fx.gameSettings.zzz, running: true } })
  assert.ok(text(running).includes(COPY.zh.gsRunning))
  assert.equal(COPY.zh.gsRunning, '游戏正在运行，修改从下次启动生效')
  const closed = await render({ app: appOf('zzz'), game: 'zzz', read: fx.gameSettings.zzz })
  const locked = (html) => (html.match(/\bdisabled\b|is-off/g) || []).length
  assert.equal(locked(running), locked(closed), 'the running game locks no control')
  assert.ok(!text(closed).includes(COPY.zh.gsRunning), 'nothing when the game is not running')
})

// an apps.json captured from a real host: the generic apps writer had saved every value of the sparxie block as text
const OWNER_ZZZ = { game: 'zzz', region: 'cn', install: 'zzz-cn', mode: 'touch', settingsMode: 'preset', restore: 'restore',
  settings: { resolution: '1920×1080', display: 'windowed', fps: '60', vsync: 'false', renderScale: '1.2', preset: 'medium', hdr: 'false' },
  options: { bgFps: '10', bgLimit: 'true', followPreset: 'false', fps: '120', fpsUnlock: 'false', preset30: '60', preset60: '1000', priority: 'normal', scale: '400', scaleOverride: 'false' } }

test('a captured stringified block draws the same page as the typed one: the stored segments are selected, the switches show their real state', async () => {
  const typedBlock = { ...OWNER_ZZZ, settings: { ...OWNER_ZZZ.settings, fps: 60, vsync: false, renderScale: 1.2, hdr: false }, options: { ...OWNER_ZZZ.options, fps: 120, bgFps: 10, bgLimit: true, followPreset: false, fpsUnlock: false, preset30: 60, preset60: 1000, scale: 400, scaleOverride: false } }
  const fromText = await render({ app: withDefaults(OWNER_ZZZ), game: 'zzz', read: fx.gameSettings.zzz })
  const fromTyped = await render({ app: withDefaults(typedBlock), game: 'zzz', read: fx.gameSettings.zzz })
  assert.equal(fromText, fromTyped)
  assert.equal(checked(fromText, 'radio', '60'), 'true', 'the 60 fps segment is selected')
  assert.equal(checked(fromText, 'radio', '30'), 'false')
  assert.equal(checked(fromText, 'radio', '中'), 'true')
  assert.equal(checked(fromText, 'radio', '1.2'), 'true')
  assert.equal(checked(fromText, 'switch', '垂直同步'), 'false', 'the text "false" is not a switch that is on')
  assert.equal(checked(fromText, 'switch', 'HDR'), 'false')
})

// A captured Star Rail application drawn with what the game really holds. The live answer of GET /api/sparxie/game-settings
// (tests/fixtures/sample-game-settings-20261112.json) and the stored application (tests/sparxie/fixtures/sample-apps-20261112.json). Before this, every item the application did not hold
// was drawn with the catalog's default (DLSS 禁用, every quality 最低 / 关), although the game had DLSS 质量 and every quality high: the host writes only the items the application holds.
const sampleAnswers = JSON.parse(readFileSync(new URL('./fixtures/sample-game-settings-20261112.json', import.meta.url), 'utf8'))
const sampleApps = JSON.parse(readFileSync(new URL('../../../../../tests/sparxie/fixtures/sample-apps-20261112.json', import.meta.url), 'utf8')).apps
const sampleApp = (name) => withDefaults(sampleApps.find((a) => a.name === name).sparxie)

test('Star Rail of the sample host: an item the application does not hold shows what the game has (DLSS 质量, the qualities 高, TAA), not the catalog default', async () => {
  const app = sampleApp('星穹铁道')
  for (const key of ['dlss', 'shadow', 'light', 'aa']) assert.ok(!(key in app.settings), `${key} is not held by the application`)
  const html = await render({ app, game: 'starrail', read: sampleAnswers['starrail-cn'] })
  const lines = text(html)
  const after = (label) => lines[lines.indexOf(label) + 1]
  assert.equal(after('NVIDIA DLSS'), '质量')
  for (const label of ['阴影质量', '光照质量', '角色质量', '场景细节', '反射质量', '特效质量', '泛光效果', '分辨率质量']) assert.equal(after(label), '高', label)
  assert.equal(checked(html, 'radio', 'TAA'), 'true')
  assert.equal(checked(html, 'switch', '地图探索中角色实时阴影'), 'true')
  assert.equal(checked(html, 'switch', '半分辨率透明'), 'false')
  assert.ok(!lines.includes('禁用'), 'no item says 禁用 while the game has DLSS')
})

test('an item the application holds keeps its own value, and says what the game has where it differs', async () => {
  const app = { ...sampleApp('星穹铁道'), settings: { ...sampleApp('星穹铁道').settings, vsync: false } }
  const lines = text(await render({ app, game: 'starrail', read: sampleAnswers['starrail-cn'] }))
  assert.ok(lines.includes('当前 开'), lines.join(' | '))
})

test('Genshin and Zenless of the sample host: the items the applications do not hold show the game\'s values', async () => {
  const giHtml = await render({ app: sampleApp('原神'), game: 'genshin', read: sampleAnswers['genshin-cn'] })
  const gi = text(giHtml)
  const giAfter = (label) => gi[gi.indexOf(label) + 1]
  // by the one control rule (1112b) the shadow quality's four short names are segments: the selected one is the game's value
  const shadow = giHtml.slice(giHtml.indexOf('阴影质量'))
  assert.equal(/role="radio"[^>]*aria-checked="true"[^>]*aria-label="([^"]*)"/.exec(shadow)?.[1], '高')
  assert.equal(checked(giHtml, 'radio', 'SMAA'), 'true')
  assert.equal(giAfter('各向异性过滤'), '8x')
  const zz = text(await render({ app: sampleApp('绝区零触屏'), game: 'zzz', read: sampleAnswers['zzz-cn'] }))
  assert.ok(zz.length > 10)
})

test('the sample Zenless application after the conversion holds nothing: its items follow the game (V-sync on) and are marked so; changing one marks it changed', async () => {
  const raw = sampleApps.find((a) => a.name === '绝区零触屏').sparxie
  // the file before the conversion: the seven default values of the first builds (nobody chose them). The one-off script leaves nothing held.
  const converted = { ...raw, settings: {} }
  delete converted.settingsMode
  const app = withDefaults(converted)
  assert.deepEqual(app.settings, {})
  const html = await render({ app, game: 'zzz', read: sampleAnswers['zzz-cn'] })
  assert.equal(checked(html, 'switch', '垂直同步'), 'true', 'the game has V-sync on')
  assert.ok(!text(html).some((l) => l.startsWith('当前')), 'nothing differs: the item is the game\'s')
  assert.ok(html.includes(COPY.zh.gsFollow) && !html.includes(COPY.zh.gsChanged), 'every item says it follows the game; none says changed')
  // one item held: it shows its own value, says changed and has the way back
  const held = withDefaults({ ...converted, settings: { vsync: false } })
  const changedHtml = await render({ app: held, game: 'zzz', read: sampleAnswers['zzz-cn'] })
  assert.equal(checked(changedHtml, 'switch', '垂直同步'), 'false', 'the application\'s own value, not the game\'s')
  assert.ok(changedHtml.includes(COPY.zh.gsChanged) && changedHtml.includes(COPY.zh.gsBack), 'marked changed, with the way back')
  assert.ok(changedHtml.includes(COPY.zh.gsAllFollow), '全部跟随游戏')
})
