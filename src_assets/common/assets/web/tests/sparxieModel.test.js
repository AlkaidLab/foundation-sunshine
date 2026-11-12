import assert from 'node:assert/strict'
import test from 'node:test'
import { COPY, copyFor, sparxieLang } from '../utils/sparxie/copy.js'
import { GAME_DEFAULTS, SPARXIE_DEFAULTS, appChips, artUrl, badgeOf, heldSettings, createBlock, defaultName, forInput, fullPath, gameFieldsOf, matchTitle, mergeInstalls, midCut, needsWriteBack, newBlock, regionOf, sourceLabel, sourcesOf, sparxieFieldsOf, sparxieOf, withDefaults } from '../utils/sparxie/model.js'

const C = COPY.zh
const genshinCn = { id: 'genshin-cn', game: 'genshin', region: 'cn', candidates: ['cn'], dir: 'D:\\Games\\Genshin Impact\\Genshin Impact Game', exe: 'YuanShen.exe', profile: 'genshin-cn' }
const bili = { id: 'p1', game: 'genshin', region: '', candidates: ['cn', 'bili'], dir: 'E:\\G', exe: 'YuanShen.exe', profile: '' }

test('a Sparxie application is a normal application with a sparxie block; anything else is not', () => {
  assert.equal(sparxieOf({ name: 'Desktop' }), null)
  assert.equal(sparxieOf({ name: 'x', sparxie: 'yes' }), null)
  assert.deepEqual(sparxieOf({ sparxie: { game: 'zzz' } }), { game: 'zzz' })
})

test('the chips under a card title are Sparxie (touch launch only), the region and the launch mode, in that order', () => {
  const chips = appChips({ sparxie: { region: 'bili', mode: 'touch' } }, C)
  assert.deepEqual(chips.map((c) => c.key), ['spx', 'region', 'mode'])
  assert.deepEqual(chips.map((c) => c.label), ['Sparxie', 'B 服', '触控'])
  const direct = appChips({ sparxie: { region: 'cn', mode: 'pc' } }, C)
  assert.deepEqual(direct.map((c) => c.key), ['region', 'mode'], 'a game app that launches directly does not carry the Sparxie mark')
  assert.equal(direct[1].label, '键鼠与手柄', 'the chip says the input a direct launch always had, never 直接启动')
  assert.deepEqual(appChips({ name: 'Steam' }, C), [])
  assert.equal(appChips({ sparxie: { mode: 'touch' } }, C).length, 2, 'an application whose region is not known carries no region chip')
})

test('the region comes from the markers; when they cannot decide, the person picks', () => {
  assert.equal(regionOf(genshinCn), 'cn')
  assert.equal(regionOf(bili), '')
  assert.equal(regionOf(bili, 'bili'), 'bili')
  assert.equal(regionOf(bili, 'global'), '', 'a pick outside the candidates is ignored')
  assert.equal(regionOf(null), '')
})

test('an install says where it came from, and the badge names the store', () => {
  assert.equal(sourceLabel({ ...genshinCn, source: 'steam' }, C), 'Steam')
  assert.equal(sourceLabel({ ...genshinCn, source: 'epic' }, COPY.en), 'Epic')
  assert.equal(sourceLabel({ ...genshinCn, source: 'hoyoplay' }, C), '米哈游启动器')
  assert.equal(sourceLabel({ ...genshinCn, source: 'official-launcher' }, C), '官方启动器')
  assert.equal(sourceLabel({ ...genshinCn, source: 'bili' }, C), 'B 站')
  assert.equal(sourceLabel({ ...genshinCn, source: 'folder' }, COPY.en), 'Folder search')
  assert.equal(sourceLabel({ ...genshinCn, source: 'manual' }, COPY.en), 'Chosen manually')
  assert.equal(sourceLabel(genshinCn, C), '', 'a host that does not send the source shows no badge')
  assert.equal(sourceLabel(null, C), '')
})

test('the name follows the game and the region; without a region it is the game alone', () => {
  assert.equal(defaultName(genshinCn, 'cn', C), '原神 · 国服')
  assert.equal(defaultName(bili, '', C), '原神')
  assert.equal(defaultName(null, '', C), '')
  assert.equal(defaultName(genshinCn, 'cn', COPY.en), 'Genshin Impact · CN')
})

test('creating is blocked without a game or a region, and while Sparxie is open when the game has to be written to it', () => {
  const free = { running: false, state: 'data' }
  assert.equal(createBlock({ install: null, region: '', store: free }), 'no-game')
  assert.equal(createBlock({ install: bili, region: '', store: free }), 'no-region')
  assert.equal(createBlock({ install: bili, region: 'bili', store: { running: true, state: 'data' } }), 'running')
  assert.equal(createBlock({ install: bili, region: 'bili', store: { running: false, state: 'unreadable' } }), 'unreadable')
  assert.equal(createBlock({ install: bili, region: 'bili', store: free }), '')
  // a game Sparxie already has needs no write, so a running Sparxie does not stop it
  assert.equal(needsWriteBack(genshinCn), false)
  assert.equal(createBlock({ install: genshinCn, region: 'cn', store: { running: true, state: 'data' } }), '')
})

test('a new block follows the game\'s own settings: it carries none of its own until one is chosen', () => {
  const b = newBlock({ install: genshinCn, region: 'cn', mode: 'touch' })
  assert.equal(b.install, 'genshin-cn'); assert.equal(b.profile, 'genshin-cn'); assert.equal(b.cover, 'official')
  assert.deepEqual(b.settings, {}); assert.deepEqual(b.options, SPARXIE_DEFAULTS)
  b.settings.fps = 30
  assert.equal(GAME_DEFAULTS.fps, 60, 'the defaults are not shared with the block')
})

test('the pages read a complete block: an application holds only the items the person changed, the rest follows the game', () => {
  const none = withDefaults({ game: 'zzz' })
  assert.deepEqual(none.settings, {}); assert.equal('settingsMode' in none, false, 'the two-mode switch is gone'); assert.equal(none.mode, 'touch'); assert.equal(none.options.priority, 'normal')
  const one = withDefaults({ game: 'zzz', settings: { fps: 30 } })
  assert.deepEqual(one.settings, { fps: 30 }, 'what was changed stays and nothing else is filled in from the defaults')
  assert.equal(one.settings.display, undefined, 'an item that is not held follows the game')
  const typed = withDefaults({ game: 'genshin', settings: { vsync: 'true', renderScale: '0.8' } })
  assert.deepEqual(typed.settings, { vsync: true, renderScale: 0.8 }, 'the generic writer stores text; the page reads the types back')
})

test('the items an application holds: every key it has (even one equal to the default), nothing for a missing or null value', () => {
  assert.deepEqual(heldSettings({ ...GAME_DEFAULTS }), { ...GAME_DEFAULTS }, 'a value that equals the default is still a change the person made')
  assert.deepEqual(heldSettings({ fps: 30, bogus: undefined, hdr: null }), { fps: 30 })
  assert.deepEqual(heldSettings(null), {})
  assert.deepEqual(heldSettings(undefined), {})
})

test('long paths keep both ends; the art URL carries the version of the official image', () => {
  assert.equal(fullPath(genshinCn), 'D:\\Games\\Genshin Impact\\Genshin Impact Game\\YuanShen.exe')
  assert.equal(midCut(fullPath(genshinCn)), 'D:\\Games\\…\\Genshin Impact Game\\YuanShen.exe')
  assert.equal(midCut('D:\\a'), 'D:\\a')
  assert.equal(artUrl('card', genshinCn, 'ab12'), '/api/sparxie/art/card/genshin-cn?v=ab12')
  assert.equal(artUrl('icon', genshinCn), '/api/sparxie/art/icon/genshin-cn')
})

test('Chinese for every Chinese locale, English for the rest; both languages have the same keys', () => {
  for (const l of ['zh', 'zh_CN', 'zh_TW', 'ZH-cn']) assert.equal(sparxieLang(l), 'zh')
  for (const l of ['en', 'de', 'ja', '']) assert.equal(l === '' ? sparxieLang(l) : sparxieLang(l), l === '' ? 'zh' : 'en')
  assert.equal(copyFor('de'), COPY.en)
  assert.deepEqual(Object.keys(COPY.zh).sort(), Object.keys(COPY.en).sort())
  for (const k of Object.keys(COPY.zh)) assert.equal(typeof COPY.zh[k], typeof COPY.en[k], k)
})

test('the statement about injection is a fact in both languages and says who carries the account risk', () => {
  assert.ok(/注入/.test(COPY.zh.notice) && /账号风险由你自行承担/.test(COPY.zh.notice))
  assert.ok(/injects code/.test(COPY.en.notice) && /risk to your account is yours/.test(COPY.en.notice))
  assert.ok(!/米哈游游戏/.test(JSON.stringify(COPY.zh)), 'the games are named by their own names, not as 米哈游游戏')
})

test('no Sparxie text talks about the program behind it: no API, interface, returned data, field or what the host answers', () => {
  const flat = (value, out = []) => { if (typeof value === 'string') out.push(value); else if (typeof value === 'function') out.push(String(value(2, 'x'))); else if (value && typeof value === 'object') Object.values(value).forEach((v) => flat(v, out)); return out }
  const meta = /接口|API|返回(的|值|数据|内容|结果|给)|字段|主机回答|主机会|控制器|这段说明|endpoint|payload|JSON|returned|the host (answers|returns|sends)|this statement/i
  for (const lang of ['zh', 'en']) {
    const bad = flat(COPY[lang]).filter((t) => meta.test(t))
    assert.deepEqual(bad, [], `${lang}: developer talk in user copy`)
  }
  assert.equal(COPY.zh.notice, COPY.zh.noticeFoot, 'one statement, said the same everywhere')
})

test('installs found by two sources are one, with both sources; the person\'s own choice and Sparxie\'s profile are sources too', () => {
  const steam = { game: 'zzz', dir: 'D:\\Steam\\ZZZ', exe: 'ZenlessZoneZero.exe', sources: ['steam'] }
  const hoyo = { game: 'zzz', dir: 'd:\\steam\\zzz', exe: 'zenlesszonezero.exe', sources: ['hoyoplay'] }
  const other = { game: 'zzz', dir: 'E:\\ZZZ', exe: 'ZenlessZoneZero.exe', sources: ['hoyoplay'] }
  const merged = mergeInstalls([{ list: [steam, other] }, { list: [hoyo], stored: true }, { list: [other], manual: true }])
  assert.equal(merged.length, 2, 'the same directory is one install; another directory is another')
  assert.deepEqual(merged[0].sources, ['steam', 'hoyoplay', 'sparxie'])
  assert.deepEqual(merged[1].sources, ['hoyoplay', 'manual'])
  assert.deepEqual(sourcesOf({ sources: [], profile: 'p1' }), ['sparxie'])
})

test('a game that only goes through 直接启动 does not need Sparxie\'s storage: only touch launch is blocked by a running Sparxie', () => {
  const install = { id: 'x', game: 'zzz', dir: 'D:\\z', exe: 'a.exe', profile: '' }
  assert.equal(createBlock({ install, region: 'cn', mode: 'touch', store: { running: true, state: 'data' } }), 'running')
  assert.equal(createBlock({ install, region: 'cn', mode: 'pc', store: { running: true, state: 'unreadable' } }), '')
})

test('a title is found by its Chinese name, English name, short name or abbreviation, whatever the case, spaces and punctuation', () => {
  const hsr = { name: '崩坏：星穹铁道', nameEn: 'Honkai: Star Rail', aliases: ['hsr', 'sr', '星铁', '崩铁'] }
  const zzz = { name: '绝区零', nameEn: 'Zenless Zone Zero', aliases: ['zzz', 'ze'] }
  for (const q of ['星铁', 'HSR', 'hsr', '星穹', '崩坏 星穹', 'honkai', 'Star Rail', 'starrail', ' ']) assert.equal(matchTitle('starrail', hsr, q), true, q)
  for (const q of ['ZZZ', 'zenless', '绝区', 'zone']) assert.equal(matchTitle('zzz', zzz, q), true, q)
  for (const q of ['zzz', '原神']) assert.equal(matchTitle('starrail', hsr, q), false, q)
  assert.equal(matchTitle('wuwa', { name: '鸣潮', nameEn: 'Wuthering Waves', aliases: ['wuwa'] }, 'wuwa'), true)
})

test('the Sparxie settings of a title are the host\'s list; a host without one gets the items Sparxie\'s profile has for the title, and the frame-rate tiers wait for the host', () => {
  const fallback = (g) => sparxieFieldsOf(null, g).map((f) => f.key)
  assert.deepEqual(fallback('genshin'), ['input', 'fpsUnlock', 'bgLimit', 'bgFps', 'priority', 'followPreset', 'scaleOverride', 'scale'])
  assert.deepEqual(fallback('starrail'), ['input', 'fpsUnlock', 'bgLimit', 'bgFps', 'priority'])
  assert.deepEqual(fallback('zzz'), ['input'], 'Sparxie has none of the frame-rate items for Zenless Zone Zero')
  assert.ok(!fallback('genshin').includes('fps') && !fallback('genshin').includes('preset30'), 'a target frame rate is a tier of the host, never a free number')
  const host = [{ key: 'x', kind: 'toggle' }]
  assert.equal(sparxieFieldsOf({ sparxieFields: host }, 'genshin'), host)
})

test('an item the host marks for touch is shown for touch only, every other item for every input', () => {
  const touchUi = sparxieFieldsOf(null, 'genshin').filter((f) => f.inputs)
  assert.deepEqual(touchUi.map((f) => f.key), ['scaleOverride', 'scale'])
  for (const f of sparxieFieldsOf(null, 'genshin')) {
    assert.equal(forInput(f, 'touch'), true)
    assert.equal(forInput(f, 'kbm'), !f.inputs, f.key)
  }
})

test('the game\'s items are the host\'s list, else the items every title of the reference launchers has', () => {
  assert.deepEqual(gameFieldsOf(null, 'zzz', ['low', 'high']).map((f) => f.key), ['resolution', 'display', 'fps', 'preset', 'renderScale', 'vsync', 'hdr'])
  assert.deepEqual(gameFieldsOf(null, 'starrail', ['low']).map((f) => f.key), ['resolution', 'display', 'fps', 'preset', 'renderScale', 'vsync'])
  assert.deepEqual(gameFieldsOf(null, 'starrail', ['low']).find((f) => f.key === 'fps').options.map((o) => o.value), [30, 60, 120])
  assert.deepEqual(gameFieldsOf({ settings: [{ key: 'bloom', kind: 'toggle' }] }, 'zzz').map((f) => f.key), ['bloom'])
})

test('the Sparxie defaults are Sparxie\'s own: the frame-rate unlock is on and the target is 120', () => {
  assert.equal(SPARXIE_DEFAULTS.fpsUnlock, true)
  assert.equal(SPARXIE_DEFAULTS.fps, 120)
  assert.equal(SPARXIE_DEFAULTS.input, 'touch')
})

test('the card chips carry the input of a Sparxie app, not a launch mode', () => {
  const app = (mode, input) => ({ name: 'x', sparxie: { game: 'genshin', region: 'cn', mode, options: input ? { input } : {} } })
  assert.equal(appChips(app('touch', 'kbm'), C).at(-1).label, C.inputKbm)
  assert.equal(appChips(app('touch'), C).at(-1).label, C.inputTouch)
  assert.equal(appChips(app('pc'), C).at(-1).label, C.inputKbm, 'an application of the first builds that starts the game directly is keyboard, mouse and gamepad')
  assert.equal(appChips(app('touch', 'auto'), C).at(-1).label, C.inputTouch, 'a stored automatic input (the removed choice) reads as touch')
})

test('the poster badge: what the block says, else on for the official cover and off for a cover of the person\'s own; the host\'s stringified flag is read back', () => {
  assert.equal(badgeOf({ cover: 'official' }), true)
  assert.equal(badgeOf({}), true)
  assert.equal(badgeOf({ cover: 'custom' }), false)
  assert.equal(badgeOf({ cover: 'custom', badge: true }), true)
  assert.equal(badgeOf({ cover: 'official', badge: false }), false)
  assert.equal(withDefaults({ game: 'genshin', badge: 'false' }).badge, false)
  assert.equal(withDefaults({ game: 'genshin', badge: 'true', cover: 'custom' }).badge, true)
})
