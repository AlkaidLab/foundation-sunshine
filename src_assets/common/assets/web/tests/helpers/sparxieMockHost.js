// A host that answers the Sparxie routes from fixtures, for the unit tests and for the screenshot harness. It follows the client's wire shapes (services/sparxieClient.js),
// so when the real host contract lands this file is the one to bring in line with it. Scenes: data | empty | unused | unreadable | absent, plus `running` (Sparxie open) and `game` (a game running).
export const INSTALLS = [
  { id: 'genshin-cn', game: 'genshin', region: 'cn', biz: 'hk4e_cn', exe: 'YuanShen.exe', dir: 'D:\\Games\\Genshin Impact\\Genshin Impact Game', version: '6.4.0', markers: ['exe', 'registry'] },
  { id: 'genshin-bili', game: 'genshin', region: 'bili', biz: 'hk4e_cn', exe: 'YuanShen.exe', dir: 'E:\\Games\\Genshin Impact Bilibili\\Genshin Impact Game', version: '6.4.0', markers: ['exe', 'sdk'] },
  { id: 'genshin-global', game: 'genshin', region: 'global', biz: 'hk4e_global', exe: 'GenshinImpact.exe', dir: 'D:\\Games\\Genshin Impact Global\\Genshin Impact Game', version: '6.4.0', markers: ['exe', 'dir'] },
  { id: 'starrail-cn', game: 'starrail', region: 'cn', biz: 'hkrpg_cn', exe: 'StarRail.exe', dir: 'D:\\Games\\Star Rail\\Game', version: '4.1.0', markers: ['registry'] },
  { id: 'starrail-global', game: 'starrail', region: 'global', biz: 'hkrpg_global', exe: 'StarRail.exe', dir: 'D:\\Games\\Star Rail Global\\Games', version: '4.1.0', markers: ['registry', 'ini'] },
  { id: 'zzz-cn', game: 'zzz', region: 'cn', biz: 'nap_cn', exe: 'ZenlessZoneZero.exe', dir: 'D:\\Games\\ZenlessZoneZero Game', version: '2.5.0', markers: ['exe'] },
  { id: 'zzz-global', game: 'zzz', region: 'global', biz: 'nap_global', exe: 'ZenlessZoneZero.exe', dir: 'E:\\SteamLibrary\\steamapps\\common\\ZenlessZoneZero', version: '2.5.0', markers: ['registry'] },
]
// titles the host does not have yet, only to check that the page lays out a longer list the host sends
export const MORE_TITLES = ['bh3', 'arknights', 'endfield', 'wuwa', 'pgr', 'ntei'].map((game, i) => ({ game, name: ['崩坏3', '明日方舟', '终末地', '鸣潮', '战双帕弥什', '异环'][i], nameEn: ["Honkai Impact 3rd","Arknights","Arknights: Endfield","Wuthering Waves","Punishing: Gray Raven","Neverness to Everness"][i], touch: false, exes: [], regions: [{ region: 'cn', exes: [`${game}.exe`] }] }))
const byId = (id) => INSTALLS.find((i) => i.id === id)
const ART = { version: 'a1b2c3d4', updated: '11-10' }
const STATEMENT = { zh: 'Sparxie 会在游戏运行时向游戏进程注入代码并修改其内存，这可能违反游戏的用户协议，由此带来的账号风险由你自行承担。', en: 'Sparxie injects code into the running game and changes its memory. This may break the game’s terms of service, and any risk to your account is yours.' }
// what a host of this build describes for each title: the game's own settings items and Sparxie's own (the page draws what is described; an item marked `inputs` is for those inputs only)
const o = (...v) => v.map((value) => (typeof value === 'object' ? value : { value }))
const GAME_FIELDS = {
  genshin: [
    { key: 'resolution', kind: 'choice', section: 'display', options: o('1280×720', '1600×900', '1920×1080', '2560×1440', '3840×2160') },
    { key: 'display', kind: 'choice', section: 'display', options: o('fullscreen', 'windowed') },
    { key: 'fps', kind: 'choice', section: 'display', options: o(30, 45, 60) },
    { key: 'vsync', kind: 'toggle', section: 'display' },
    { key: 'preset', kind: 'choice', section: 'quality', options: o('low', 'medium', 'high') },
    { key: 'renderScale', kind: 'slider', section: 'quality', min: 0.6, max: 1.4, step: 0.1, decimals: 1, marks: [0.6, 1, 1.4] },
    { key: 'shadow', kind: 'choice', section: 'quality', label: '阴影质量', labelEn: 'Shadow quality', options: o({ value: 1, label: '低', labelEn: 'Low' }, { value: 2, label: '中', labelEn: 'Medium' }, { value: 3, label: '高', labelEn: 'High' }) },
    { key: 'hdr', kind: 'toggle', section: 'quality' },
  ],
  starrail: [
    { key: 'resolution', kind: 'choice', section: 'display', options: o('1280×720', '1600×900', '1920×1080', '2560×1440', '3840×2160') },
    { key: 'display', kind: 'choice', section: 'display', options: o('fullscreen', 'windowed') },
    { key: 'fps', kind: 'choice', section: 'display', options: o(30, 60, 120) },
    { key: 'vsync', kind: 'toggle', section: 'display' },
    { key: 'preset', kind: 'choice', section: 'quality', options: o('low', 'medium', 'high') },
    { key: 'renderScale', kind: 'slider', section: 'quality', min: 0.6, max: 1.4, step: 0.1, decimals: 1, marks: [0.6, 1, 1.4] },
  ],
  zzz: [
    { key: 'resolution', kind: 'choice', section: 'display', options: o('1280×720', '1600×900', '1920×1080', '2560×1440', '3840×2160') },
    { key: 'display', kind: 'choice', section: 'display', options: o('fullscreen', 'windowed') },
    { key: 'fps', kind: 'choice', section: 'display', options: o(30, 60, 0) },
    { key: 'vsync', kind: 'toggle', section: 'display' },
    { key: 'preset', kind: 'choice', section: 'quality', options: o('low', 'medium', 'high') },
    { key: 'renderScale', kind: 'slider', section: 'quality', min: 0.8, max: 1.2, step: 0.2, decimals: 1, marks: [0.8, 1, 1.2] },
    { key: 'aa', kind: 'choice', section: 'quality', label: '抗锯齿', labelEn: 'Anti-aliasing', options: o({ value: 'off', label: '关', labelEn: 'Off' }, { value: 'taa', label: 'TAA', labelEn: 'TAA' }, { value: 'smaa', label: 'SMAA', labelEn: 'SMAA' }) },
    { key: 'bloom', kind: 'toggle', section: 'quality', label: '泛光', labelEn: 'Bloom' },
    { key: 'motionBlur', kind: 'toggle', section: 'quality', label: '动态模糊', labelEn: 'Motion blur' },
    { key: 'hdr', kind: 'toggle', section: 'quality' },
  ],
}
const TIERS = [60, 90, 120, 144, 165, 240, 'display']
const SPARXIE_FIELDS = {
  genshin: [
    { key: 'input', kind: 'choice', section: 'input', options: o('touch', 'kbm'), default: 'touch' },
    { key: 'fpsUnlock', kind: 'toggle', section: 'fps', default: true },
    { key: 'fps', kind: 'choice', section: 'fps', options: o(...TIERS), default: 120, enabledBy: 'fpsUnlock' },
    { key: 'bgLimit', kind: 'toggle', section: 'fps', default: true },
    { key: 'bgFps', kind: 'slider', section: 'fps', min: 10, max: 1000, step: 1, scale: 'log', marks: [10, 30, 60], unit: ' FPS', default: 10, enabledBy: 'bgLimit' },
    { key: 'priority', kind: 'choice', section: 'advanced', options: o('belowNormal', 'normal', 'aboveNormal', 'high'), default: 'normal' },
    { key: 'followPreset', kind: 'toggle', section: 'advanced', default: false },
    { key: 'preset30', kind: 'choice', section: 'advanced', options: o(60, 90, 120, 144), default: 60, enabledBy: 'followPreset' },
    { key: 'preset60', kind: 'choice', section: 'advanced', options: o(120, 144, 165, 240, 1000), default: 1000, enabledBy: 'followPreset' },
    { key: 'scaleOverride', kind: 'toggle', section: 'ui', default: false, inputs: ['touch'] },
    { key: 'scale', kind: 'slider', section: 'ui', min: 100, max: 500, step: 5, marks: [100, 250, 400, 500], unit: '%', default: 400, enabledBy: 'scaleOverride', inputs: ['touch'] },
  ],
  starrail: [
    { key: 'input', kind: 'choice', section: 'input', options: o('touch', 'kbm'), default: 'touch' },
    { key: 'fpsUnlock', kind: 'toggle', section: 'fps', default: true },
    { key: 'fps', kind: 'choice', section: 'fps', options: o(...TIERS), default: 120, enabledBy: 'fpsUnlock' },
    { key: 'bgLimit', kind: 'toggle', section: 'fps', default: true },
    { key: 'bgFps', kind: 'slider', section: 'fps', min: 10, max: 1000, step: 1, scale: 'log', marks: [10, 30, 60], unit: ' FPS', default: 10, enabledBy: 'bgLimit' },
    { key: 'priority', kind: 'choice', section: 'advanced', options: o('belowNormal', 'normal', 'aboveNormal', 'high'), default: 'normal' },
  ],
  zzz: [{ key: 'input', kind: 'choice', section: 'input', options: o('touch', 'kbm'), default: 'touch' }],
}
const CATALOG = [
  { game: 'genshin', settings: GAME_FIELDS.genshin, sparxie: { fields: SPARXIE_FIELDS.genshin }, name: '原神', nameEn: 'Genshin Impact', aliases: ['gi', 'ys', 'yuanshen'], touch: true, exes: ['YuanShen.exe', 'GenshinImpact.exe'], regions: [{ region: 'cn', exes: ['YuanShen.exe'] }, { region: 'bili', exes: ['YuanShen.exe'] }, { region: 'global', exes: ['GenshinImpact.exe'] }] },
  { game: 'starrail', settings: GAME_FIELDS.starrail, sparxie: { fields: SPARXIE_FIELDS.starrail }, name: '崩坏：星穹铁道', nameEn: 'Honkai: Star Rail', aliases: ['hsr', 'sr', '星铁', '崩铁'], touch: true, exes: ['StarRail.exe'], regions: ['cn', 'bili', 'global'].map((region) => ({ region, exes: ['StarRail.exe'] })) },
  { game: 'zzz', settings: GAME_FIELDS.zzz, sparxie: { fields: SPARXIE_FIELDS.zzz }, name: '绝区零', nameEn: 'Zenless Zone Zero', aliases: ['zzz', 'ze', 'zenless'], touch: true, exes: ['ZenlessZoneZero.exe', 'ZenlessZoneZeroBeta.exe'], regions: ['cn', 'bili', 'global'].map((region) => ({ region, exes: ['ZenlessZoneZero.exe', 'ZenlessZoneZeroBeta.exe'] })) },
]
// the drives and folders the browse list shows (a fake file system of the signed-in person)
export const TREE = {
  '': [{ name: 'Windows (C:)', path: 'C:\\', kind: 'drive' }, { name: 'Games (D:)', path: 'D:\\', kind: 'drive' }],
  'D:\\': [{ name: 'Games', path: 'D:\\Games', kind: 'dir' }, { name: 'Tools', path: 'D:\\Tools', kind: 'dir' }],
  'D:\\Games': [{ name: 'Star Rail', path: 'D:\\Games\\Star Rail', kind: 'dir' }, { name: 'StarRail.exe', path: 'D:\\Games\\StarRail.exe', kind: 'file', known: true }, { name: 'launcher.exe', path: 'D:\\Games\\launcher.exe', kind: 'file' }],
  'D:\\Tools': [],
}
const SOURCES = { 'genshin-global': ['hoyoplay'], 'starrail-global': ['hoyoplay'], 'zzz-global': ['steam', 'hoyoplay'], 'genshin-cn': ['hoyoplay'] }
const withArt = (i, profile) => ({ ...i, candidates: [i.region], profile: profile || '', sources: SOURCES[i.id] || [], art: ART })

// the folders the picker can "choose" in the scenes of the add-local-game flow
export const FOLDERS = {
  'region-add': { install: { ...byId('genshin-global'), markers: ['exe', 'dir', 'ini'] }, has_game: true },
  ambiguous: { install: { ...byId('genshin-bili'), region: '', candidates: ['cn', 'bili'], markers: ['exe', 'dir'] }, has_game: false },
  single: { install: { ...byId('zzz-global'), markers: ['exe', 'dir'] }, has_game: false },
  several: { install: { ...byId('genshin-global'), markers: ['exe', 'dir'] }, has_game: true },
  unrecognised: null,
}

export function createMockHost({ scene = 'data', running = false, game = false, folder = 'region-add', warned = false, failCreate = false, newArt = false, titles = 'three', fields = 'full', touchUiSwitch = false } = {}) {
  const st = { scene, touchUiSwitch, running, game, folder, warned, failCreate, newArt, artVersion: '', opened: 0, profiles: scene === 'data' || scene === 'running' ? ['genshin-cn', 'starrail-cn', 'zzz-cn'] : [], calls: [] }
  const err = (status, code, message, data) => Object.assign(new Error(message || code), { status, code, data })
  const storeState = () => (st.scene === 'unused' ? 'unused' : st.scene === 'unreadable' ? 'unreadable' : st.profiles.length ? 'data' : 'empty')
  const stored = () => st.profiles.map((id) => withArt(byId(id), id))
  const detected = () => (st.scene === 'absent' ? [] : ['genshin-global', 'starrail-global', 'zzz-global'].filter((id) => !st.profiles.includes(id)).map((id) => withArt(byId(id), '')))

  async function handle(method, url, body = {}) {
    st.calls.push([method, url, body])
    const u = new URL(url, 'http://host')
    const p = u.pathname
    if (st.scene === 'nocomp' && p === '/api/sparxie/status') return { installed: false, running: false, version: '', store: { state: 'unused', profiles: 0 }, warned: true, statement: STATEMENT.zh, statement_en: STATEMENT.en }
    if (st.scene === 'absent' && p.startsWith('/api/sparxie') && p !== '/api/sparxie/status') throw err(409, 'absent')
    if (st.scene === 'absent' && p === '/api/sparxie/status') return { installed: false, running: false, version: '', store: { state: 'unused', profiles: 0 }, warned: true, statement: STATEMENT.zh, statement_en: STATEMENT.en }
    if (p === '/api/sparxie/status') return { installed: true, running: st.running, version: '1.4.2', capabilities: st.touchUiSwitch ? ['touch-ui-switch'] : [], touchUiSwitch: !!st.touchUiSwitch, store: { state: storeState(), profiles: st.profiles.length }, warned: st.warned, statement: STATEMENT.zh, statement_en: STATEMENT.en }
    if (p === '/api/sparxie/games') return { stored: stored(), detected: detected(), catalog: (titles === 'many' ? [...CATALOG, ...MORE_TITLES] : CATALOG).map(({ settings, sparxie, ...rest }) => (fields === 'none' ? rest : { settings, sparxie, ...rest })), art: Object.fromEntries(INSTALLS.filter((i) => i.id.endsWith('-cn') || i.id.endsWith('-global')).map((i) => [i.id, { version: st.artVersion || ART.version }])) }
    if (p === '/api/sparxie/pick') throw err(409, 'picker_unavailable')
    if (p === '/api/sparxie/scan') {
      const f = FOLDERS[st.folder]
      if (st.scene === 'nouser') throw err(409, 'no_user_session')
      if (!f || (body.game && f.install.game !== body.game)) throw err(404, 'unrecognised', 'none', { timed_out: st.folder === 'timeout' })
      const list = st.folder === 'several' ? [FOLDERS['region-add'].install, { ...FOLDERS['region-add'].install, dir: 'E:\\Games\\Genshin Global 2' }] : [f.install]
      return { ok: true, timed_out: false, installs: list.map((i) => ({ ...i, candidates: i.candidates || [i.region], profile: '', art: ART })) }
    }
    if (p === '/api/sparxie/browse') {
      if (st.scene === 'nouser') throw err(409, 'no_user_session')
      const rows = TREE[body.path || '']
      if (!rows) throw err(404, 'not_found')
      const parent = !body.path || /^[A-Z]:\\$/.test(body.path) ? '' : body.path.replace(/\\[^\\]+$/, '').replace(/^([A-Z]:)$/, '$1\\')
      return { ok: true, path: body.path || '', parent, truncated: false, entries: rows.filter((r) => body.files || r.kind !== 'file') }
    }
    if (p === '/api/sparxie/open') { st.opened = (st.opened || 0) + 1; return { ok: true, already_running: !!st.running } }
    if (p === '/api/sparxie/art/refresh') { const changed = st.newArt && st.artVersion !== 'f00dbabe'; if (changed) st.artVersion = 'f00dbabe'; return { ok: true, updated: changed ? 1 : 0 } }
    if (p === '/api/sparxie/identify' && /zenlesszonezero\.exe$/i.test(String(body.path || ''))) return { ok: true, has_game: true, install: { ...withArt(byId('zzz-global'), ''), dir: String(body.path).replace(/[\\/][^\\/]*$/, ''), markers: ['exe', 'registry'] } }
    if (p === '/api/sparxie/identify') {
      const f = FOLDERS[st.folder]
      if (!f) throw err(404, 'unrecognised')
      if (body.game && f.install.game !== body.game) throw err(409, 'wrong_game', 'wrong', { allowed: CATALOG.find((c) => c.game === body.game).exes })
      return { install: { ...f.install, candidates: f.install.candidates || [f.install.region], profile: '', art: ART }, has_game: f.has_game }
    }
    if (method === 'PUT' && /^\/api\/sparxie\/apps\/\d+$/.test(p)) { if (st.failCreate) throw err(500, 'write_failed'); return { ok: true, saved: true, wrote: body.mode === 'touch', app: { name: body.name || 'x', sparxie: { mode: body.mode || 'pc', settings: body.settings || {} } } } }
    if (p === '/api/sparxie/apps') {
      if (st.failCreate) throw err(500, 'write_failed')
      const inst = byId(body.install) || (FOLDERS[st.folder] && FOLDERS[st.folder].install)
      if (!inst) throw err(400, 'unrecognised')
      const needs = !st.profiles.includes(inst.id)
      if (needs && st.running) throw err(409, 'sparxie_running')
      if (needs && st.scene === 'unreadable') throw err(409, 'config_unreadable')
      if (needs) st.profiles.push(inst.id)
      return { ok: true, saved: true, index: 3, wrote: needs, write_back: { state: needs ? 'written' : (body.mode === 'pc' ? 'skipped' : 'skipped'), profile: inst.id }, app: { name: body.name, cmd: body.mode === 'pc' ? `"${inst.dir}\\${inst.exe}"` : `"C:\\Sparxie\\Sparxie.exe" --profile ${inst.id}`, 'image-path': `sparxie-${inst.biz}.png`, elevated: body.mode !== 'pc', sparxie: { game: inst.game, region: body.region || inst.region, install: inst.id, profile: inst.id, mode: body.mode, cover: 'official', restore: 'restore' } } }
    }
    if (p === '/api/sparxie/profiles') return { profiles: ['genshin-cn', 'genshin-global', 'genshin-cn-144', 'starrail-cn', 'starrail-global', 'zzz-cn'].filter(() => st.profiles.length).map((id) => ({ id, name: id === 'genshin-cn-144' ? '144 FPS' : '', taken: ['genshin-cn', 'genshin-global', 'starrail-cn'].includes(id), install: withArt(byId(id.replace('-144', '')), id) })) }
    if (p === '/api/sparxie/import') return { ok: true, saved: true, apps: (body.ids || []).map((id) => ({ name: id, sparxie: { install: id.replace('-144', ''), profile: id, mode: 'touch' } })) }
    if (p === '/api/sparxie/game-settings') return { ok: true, unavailable: st.scene === 'unavailable' ? ['hdr'] : [], running: st.game, values: { resolution: '2560×1440', display: 'windowed', fps: 60, vsync: true, renderScale: 1, preset: 'high', hdr: false } }
    if (p === '/api/sparxie/warning') { st.warned = true; return { ok: true } }
    throw err(404, 'not_found')
  }
  return { state: st, handle, client: { get: (url) => handle('GET', url), post: (url, body) => handle('POST', url, body), put: (url, body) => handle('PUT', url, body) } }
}
