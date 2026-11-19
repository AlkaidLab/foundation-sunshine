import assert from 'node:assert/strict'
import test from 'node:test'
import { ABSENT, SparxieError, createClient, normalizeCatalog, normalizeFields, normalizeInstall, normalizeStatus } from '../services/sparxieClient.js'
import { createMockHost } from './helpers/sparxieMockHost.js'
import { COPY } from "../utils/sparxie/copy.js"

const clientOf = (opts) => { const host = createMockHost(opts); return { host, client: createClient(host.client) } }

test('a host without the component, or without the route, is absent: nothing of Sparxie is shown', async () => {
  const { client } = clientOf({ scene: 'absent' })
  const absent = await client.status()
  assert.equal(absent.installed, false); assert.equal(absent.warned, true)
  assert.ok(absent.statement.zh && absent.statement.en, 'the host sends its statement even when the component is absent')
  assert.equal(ABSENT.installed, false)
  assert.equal(normalizeStatus(null), ABSENT)
  assert.equal(normalizeStatus({ installed: false }).installed, false)
})

test('the status carries whether Sparxie is open, the state of its storage and whether the statement was shown', async () => {
  const data = await clientOf({ scene: 'data' }).client.status()
  assert.equal(data.installed, true); assert.equal(data.store.state, 'data'); assert.equal(data.store.profiles, 3); assert.equal(data.running, false); assert.equal(data.warned, false)
  assert.equal((await clientOf({ scene: 'empty' }).client.status()).store.state, 'empty')
  assert.equal((await clientOf({ scene: 'unused' }).client.status()).store.state, 'unused')
  assert.equal((await clientOf({ scene: 'unreadable' }).client.status()).store.state, 'unreadable')
  assert.equal((await clientOf({ scene: 'data', running: true }).client.status()).running, true)
})

test('the status says whether the installed Sparxie can run a session without its touch interface (keyboard, mouse and gamepad)', async () => {
  assert.equal(ABSENT.touchUiSwitch, false)
  assert.equal((await clientOf({ scene: 'data' }).client.status()).touchUiSwitch, false, 'a build without capabilities.json has no switch')
  assert.equal((await clientOf({ scene: 'data', touchUiSwitch: true }).client.status()).touchUiSwitch, true)
  assert.equal(normalizeStatus({ installed: true, touchUiSwitch: 'yes' }).touchUiSwitch, false, 'only a real true counts')
})

test('the games Sparxie stores come apart from the ones the host found', async () => {
  const { client } = clientOf({ scene: 'data' })
  const games = await client.games()
  assert.deepEqual(games.stored.map((g) => g.id), ['genshin-cn', 'starrail-cn', 'zzz-cn'])
  assert.deepEqual(games.detected.map((g) => g.id), ['genshin-global', 'starrail-global', 'zzz-global'])
  assert.ok(games.stored.every((g) => g.profile) && games.detected.every((g) => !g.profile))
  assert.equal(games.stored[0].art.version, 'a1b2c3d4')
  const empty = await clientOf({ scene: 'empty' }).client.games()
  assert.equal(empty.stored.length, 0); assert.equal(empty.detected.length, 3)
})

test('an executable is identified with its region, with the candidates when the markers cannot tell, and kept to the game that was chosen', async () => {
  const regionAdd = clientOf({ scene: 'data', folder: 'region-add' })
  const a = await regionAdd.client.identify('E:\\Games\\GenshinImpact.exe', { game: 'genshin', region: 'global' })
  assert.equal(a.install.region, 'global'); assert.equal(a.hasGame, true); assert.deepEqual(a.install.markers, ['exe', 'dir', 'ini'])
  const amb = clientOf({ scene: 'data', folder: 'ambiguous' })
  const b = await amb.client.identify('E:\\x')
  assert.deepEqual(b.install.candidates, ['cn', 'bili']); assert.equal(b.install.region, '', 'no region is claimed while it is open')
  assert.deepEqual(await clientOf({ scene: 'data', folder: 'unrecognised' }).client.identify('E:\\x'), { error: 'unrecognised', allowed: [] })
  const wrong = await regionAdd.client.identify('E:\\Games\\GenshinImpact.exe', { game: 'starrail', region: 'cn' })
  assert.equal(wrong.error, 'wrong_game'); assert.deepEqual(wrong.allowed, ['StarRail.exe'], 'the names that fit are the host\'s answer')
})

test('the host never opens a dialog: the client has no picker call, and the host answers picker_unavailable if one is asked', async () => {
  const { client, host } = clientOf({ scene: 'data' })
  assert.equal(client.pick, undefined)
  await assert.rejects(() => host.client.post('/api/sparxie/pick', { kind: 'folder' }), (e) => e.code === 'picker_unavailable')
})

test('a folder is searched by the host: every install of the nearest level comes back, and why nothing did', async () => {
  const one = await clientOf({ scene: 'data', folder: 'region-add' }).client.scan('E:\\Games', { game: 'genshin', region: 'global' })
  assert.equal(one.installs.length, 1); assert.equal(one.installs[0].region, 'global'); assert.equal(one.timedOut, false)
  const several = await clientOf({ scene: 'data', folder: 'several' }).client.scan('E:\\Games', { game: 'genshin', region: 'global' })
  assert.equal(several.installs.length, 2)
  assert.deepEqual(await clientOf({ scene: 'data', folder: 'unrecognised' }).client.scan('E:\\x', { game: 'zzz' }), { error: 'unrecognised', timedOut: false })
  assert.equal((await clientOf({ scene: 'data', folder: 'timeout' }).client.scan('E:\\x', { game: 'zzz' })).timedOut, true)
  await assert.rejects(() => clientOf({ scene: 'nouser', folder: 'region-add' }).client.scan('E:\\x'), (e) => e.code === 'no_user_session')
})

test('the browse list is read-only and begins at the drives of the signed-in person', async () => {
  const { client, host } = clientOf({ scene: 'data' })
  const roots = await client.browse('')
  assert.deepEqual(roots.entries.map((e) => e.kind), ['drive', 'drive']); assert.equal(roots.parent, '')
  const games = await client.browse('D:\\Games', { files: true })
  assert.deepEqual(games.entries.map((e) => e.name), ['Star Rail', 'StarRail.exe', 'launcher.exe']); assert.ok(games.entries[1].known)
  assert.equal(games.parent, 'D:\\')
  assert.equal((await client.browse('D:\\Games')).entries.length, 1, 'folders only unless files are asked for')
  assert.ok(host.state.calls.every(([method, url]) => method === 'POST' || method === 'GET') && !host.state.calls.some(([, url]) => /delete|write|remove/.test(url)))
})

test('Sparxie is opened by the host in the signed-in session, and a second open reports that it already runs', async () => {
  const { client, host } = clientOf({ scene: 'data' })
  assert.equal((await client.openSparxie()).alreadyRunning, false); assert.equal(host.state.opened, 1)
  assert.equal((await clientOf({ scene: 'data', running: true }).client.openSparxie()).alreadyRunning, true)
})

test('the games carry the host\'s table of executables per game and region, and an older host gets the one of this build', async () => {
  const games = await clientOf({ scene: 'data' }).client.games()
  assert.deepEqual(games.catalog.genshin.regions.global, ['GenshinImpact.exe']); assert.deepEqual(games.catalog.genshin.regions.cn, ['YuanShen.exe'])
  const old = normalizeCatalog(undefined)
  assert.deepEqual(old.zzz.regions.bili, ['ZenlessZoneZero.exe', 'ZenlessZoneZeroBeta.exe'])
  assert.deepEqual(Object.keys(old), ['genshin', 'starrail', 'zzz'])
})

test('creating adds a game Sparxie does not have to its storage; a game it has is left alone', async () => {
  const { client, host } = clientOf({ scene: 'data' })
  const known = await client.createApp({ install: 'genshin-cn', region: 'cn', mode: 'touch', name: '原神 · 国服' })
  assert.equal(known.wrote, false); assert.equal(known.app.sparxie.mode, 'touch'); assert.equal(known.saved, true, 'the host appends the application itself'); assert.equal(known.index, 3)
  const added = await client.createApp({ install: 'genshin-global', region: 'global', mode: 'pc', name: 'Global' })
  assert.equal(added.wrote, true); assert.equal(added.state, 'written'); assert.ok(host.state.profiles.includes('genshin-global'))
  assert.ok(!/Sparxie\.exe/.test(added.app.cmd), 'the keyboard and gamepad mode starts the game itself')
})

test('Sparxie being open blocks the write, with its own error code and without touching the storage', async () => {
  const { client, host } = clientOf({ scene: 'data', running: true })
  await assert.rejects(() => client.createApp({ install: 'genshin-global', region: 'global', mode: 'touch', name: 'x' }), (e) => e instanceof SparxieError && e.code === 'sparxie_running')
  assert.ok(!host.state.profiles.includes('genshin-global'))
  // a game that is already in Sparxie needs no write, so it still goes through
  assert.equal((await client.createApp({ install: 'genshin-cn', region: 'cn', mode: 'touch', name: 'x' })).wrote, false)
  const bad = clientOf({ scene: 'unreadable' })
  await assert.rejects(() => bad.client.createApp({ install: 'genshin-global', region: 'global', mode: 'touch', name: 'x' }), (e) => e.code === 'config_unreadable')
})

test('the game settings say whether the game runs, so the page can lock them', async () => {
  assert.equal((await clientOf({ scene: 'data', game: true }).client.gameSettings('genshin-cn')).running, true)
  const free = await clientOf({ scene: 'data' }).client.gameSettings('genshin-cn')
  assert.equal(free.running, false); assert.equal(free.values.resolution, '2560×1440'); assert.deepEqual(free.unavailable, [])
  assert.deepEqual((await clientOf({ scene: 'unavailable' }).client.gameSettings('genshin-cn')).unavailable, ['hdr'])
})

test('the profiles Sparxie has: those that belong to an application are marked; importing returns the applications to save', async () => {
  const { client } = clientOf({ scene: 'data' })
  const profiles = await client.profiles()
  assert.ok(profiles.find((p) => p.id === 'genshin-cn').taken); assert.ok(!profiles.find((p) => p.id === 'zzz-cn').taken)
  const imported = await client.importProfiles(['zzz-cn'])
  assert.equal(imported.apps.length, 1); assert.equal(imported.saved, true)
  assert.deepEqual(await clientOf({ scene: 'empty' }).client.profiles(), [])
})

test('the statement is recorded once', async () => {
  const { client, host } = clientOf({ scene: 'data' })
  assert.equal(host.state.warned, false)
  await client.acknowledgeNotice()
  assert.equal(host.state.warned, true); assert.equal((await client.status()).warned, true)
})

test('what the host sends is read defensively: a title the host names is kept, a malformed one, unknown regions and unknown fields are dropped', () => {
  assert.equal(normalizeInstall({ game: 'Tears of!', region: 'cn' }), null)
  assert.equal(normalizeInstall({ game: 'wuthering_waves', region: 'cn', sources: ['steam', ''] }).game, 'wuthering_waves', 'the list of titles is the host\'s, not this build\'s')
  assert.deepEqual(normalizeInstall({ game: 'zzz', region: 'cn', sources: ['steam', '', 'hoyoplay'] }).sources, ['steam', 'hoyoplay'])
  assert.equal(normalizeInstall(null), null)
  const i = normalizeInstall({ id: 'a', game: 'zzz', region: 'mars', candidates: ['cn', 'mars'], markers: ['exe'], dir: 'D:\\x', exe: 'ZenlessZoneZero.exe' })
  assert.deepEqual(i.candidates, ['cn']); assert.equal(i.region, 'cn'); assert.equal(i.art.version, '')
})

test('the posters carry their version: games say which version each has, and a refresh says whether any changed', async () => {
  const { client } = clientOf({ scene: 'data', newArt: true })
  const before = await client.games()
  assert.equal(before.art['genshin-cn'], 'a1b2c3d4')
  assert.equal(await client.refreshArt(), 1, 'the host fetched a new poster')
  assert.equal((await client.games()).art['genshin-cn'], 'f00dbabe', 'the new poster is a new address')
  assert.equal(await client.refreshArt(), 0)
  assert.deepEqual((await clientOf({ scene: 'data' }).client.games()).art['starrail-cn'], 'a1b2c3d4'.length ? 'a1b2c3d4' : '')
})

test('creating sends the game settings that were changed (none: {}), the restore policy and a custom cover', async () => {
  const { client, host } = clientOf({ scene: 'data' })
  await client.createApp({ install: 'genshin-cn', path: 'D:\\Games\\YuanShen.exe', region: 'cn', mode: 'touch', name: 'plain', cover: 'official', image: 'x.png', hero: 'y.png' })
  const plain = host.state.calls.at(-1)[2]
  assert.deepEqual(plain.settings, {}, 'an application that changed nothing follows the game: it sends no items'); assert.equal('settingsMode' in plain, false, 'the two-mode switch is gone'); assert.equal('restore' in plain, false)
  assert.equal('cover' in plain, false); assert.equal('image-path' in plain, false)
  await client.createApp({ install: 'genshin-cn', path: 'D:\\Games\\YuanShen.exe', region: 'cn', mode: 'touch', name: 'own', settings: { fps: 60 }, restore: 'keep', cover: 'custom', image: 'C:\\p.png', hero: 'https://x/h.jpg' })
  const own = host.state.calls.at(-1)[2]
  assert.equal('settingsMode' in own, false); assert.deepEqual(own.settings, { fps: 60 }); assert.equal(own.restore, 'keep'); assert.equal(own.cover, 'custom'); assert.equal(own['image-path'], 'C:\\p.png'); assert.equal(own.hero, 'https://x/h.jpg')
})

test('the game\'s current settings can be read for an install the host does not store yet, by its executable and region', async () => {
  const { client, host } = clientOf({ scene: 'data' })
  await client.gameSettings('genshin-open-1234', { exe: 'E:\\Games\\GenshinImpact.exe', region: 'global' })
  const url = host.state.calls.at(-1)[1]
  assert.ok(url.includes('install=genshin-open-1234') && url.includes('exe=E%3A%5CGames%5CGenshinImpact.exe') && url.includes('region=global'))
})

test('a game app is changed in place: the page sends what it changed to PUT /api/sparxie/apps/<index>', async () => {
  const { client, host } = clientOf({ scene: 'data' })
  const saved = await client.updateApp(3, { name: 'Genshin', mode: 'touch', settings: { fps: 60 } })
  assert.equal(saved.index, 3); assert.equal(saved.wrote, true); assert.deepEqual(saved.app.sparxie.settings, { fps: 60 }); assert.equal('settingsMode' in saved.app.sparxie, false)
  const call = host.state.calls.at(-1)
  assert.equal(call[0], 'PUT'); assert.equal(call[1], '/api/sparxie/apps/3'); assert.equal(call[2].mode, 'touch')
  await assert.rejects(() => clientOf({ scene: 'data', failCreate: true }).client.updateApp(0, { name: 'x' }), (e) => e.code === 'write_failed')
})

test('the titles carry their names and abbreviations, and the list is the host\'s', async () => {
  const games = await clientOf({ scene: 'data' }).client.games()
  assert.deepEqual(Object.keys(games.catalog), ['genshin', 'starrail', 'zzz'])
  assert.deepEqual(games.catalog.starrail.aliases, ['hsr', 'sr', '星铁', '崩铁']); assert.equal(games.catalog.zzz.nameEn, 'Zenless Zone Zero'); assert.equal(games.catalog.zzz.touch, true)
  const many = await clientOf({ scene: 'data', titles: 'many' }).client.games()
  assert.equal(Object.keys(many.catalog).length, 9); assert.equal(many.catalog.wuwa.touch, false, 'a title Sparxie does not support has no touch launch')
})

test('the described items of a title are read as the host sends them; what cannot be drawn is dropped', () => {
  const list = normalizeFields([
    { key: 'fps', kind: 'choice', options: [30, { value: 60, label: '60', labelEn: '60' }], default: 60 },
    { key: 'scale', kind: 'slider', min: 100, max: 500, step: 5, inputs: ['touch'], enabledBy: 'scaleOverride', scale: 'log' },
    { key: 'vsync', kind: 'toggle' },
    { key: 'bad key', kind: 'toggle' }, { key: 'nokind', kind: 'weird' }, { key: 'empty', kind: 'choice', options: [] }, { key: 'flat', kind: 'slider', min: 5, max: 5 },
  ])
  assert.deepEqual(list.map((f) => f.key), ['fps', 'scale', 'vsync'])
  assert.deepEqual(list[0].options.map((o) => o.value), [30, 60])
  assert.equal(list[0].def, 60)
  assert.deepEqual(list[1].inputs, ['touch'])
  assert.equal(list[1].log, true)
  assert.equal(list[1].enabledBy, 'scaleOverride')
  assert.equal(normalizeFields(undefined), null)
  assert.equal(normalizeFields([]), null)
})

test('the catalog carries the items the host describes for each title, and none for a host that sends none', async () => {
  const { client } = clientOf()
  const rich = (await client.games()).catalog
  assert.ok(rich.genshin.settings.some((f) => f.key === 'fps') && rich.genshin.sparxieFields.some((f) => f.key === 'fpsUnlock'))
  assert.deepEqual(rich.zzz.sparxieFields.map((f) => f.key), ['input'], 'Zenless Zone Zero has no frame-rate items in Sparxie')
  assert.ok(rich.genshin.sparxieFields.find((f) => f.key === 'scale').inputs.includes('touch'))
  const bare = normalizeCatalog([{ game: 'genshin', name: 'x', regions: [] }])
  assert.equal(bare.genshin.settings, null)
  assert.equal(bare.genshin.sparxieFields, null)
})

test('creating sends the poster badge for a touch application only, and the copy of the option is in both languages', async () => {
  const { client, host } = clientOf({ scene: 'data' })
  await client.createApp({ install: 'genshin-cn', path: 'D:\\Games\\YuanShen.exe', region: 'cn', mode: 'touch', name: 'on', cover: 'official', badge: true })
  assert.equal(host.state.calls.at(-1)[2].badge, true)
  await client.createApp({ install: 'genshin-cn', path: 'D:\\Games\\YuanShen.exe', region: 'cn', mode: 'touch', name: 'off', cover: 'custom', badge: false, image: 'C:\\p.png' })
  assert.equal(host.state.calls.at(-1)[2].badge, false)
  await client.createApp({ install: 'genshin-cn', path: 'D:\\Games\\YuanShen.exe', region: 'cn', mode: 'pc', name: 'direct', badge: true })
  assert.equal('badge' in host.state.calls.at(-1)[2], false, 'a game that starts directly has no badge')
  await client.createApp({ install: 'genshin-cn', path: 'D:\\Games\\YuanShen.exe', region: 'cn', mode: 'touch', name: 'unset' })
  assert.equal('badge' in host.state.calls.at(-1)[2], false, 'unset: the host decides (on for the official cover)')
  for (const lang of ['zh', 'en']) for (const key of ['posterBadge', 'posterBadgeOfficial', 'posterBadgeCustom']) assert.ok(COPY[lang][key], `${lang}.${key}`)
})
