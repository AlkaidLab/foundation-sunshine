import './helpers/foundationUi.mjs'
import test from 'node:test'
import assert from 'node:assert/strict'
import QRCode from 'qrcode'

const { GUIDE_CLIENTS, GUIDE_DEVICES, qrPath, readProgress, writeProgress } = await import('foundation-ui/vue/guide.js')
const { CLIENT_RESOURCES, HARMONY_CLIENT_URL } = await import('../config/resources.js')
const { CLIENT_PLATFORMS } = await import('../config/clientDownloads.js')
const { resetBootstrapDataCache } = await import('../config/bootstrapData.js')
const { postCredentials } = await import('../composables/useWelcome.js')
const { createAccountCallback, useGuide } = await import('../composables/useGuide.js')
const { decideAutoOpen, decideGuideEntry } = await import('../utils/guideLauncher.js')
const { markSetupCompleted, saveSetupChoices } = await import('../services/setupWizardService.js')

// A host with routes: each entry is a body or a function (url, init) -> body | Response
function host(routes, calls = []) {
  const original = globalThis.fetch
  globalThis.fetch = async (url, init = {}) => {
    calls.push({ url: String(url), method: init.method || 'GET', body: typeof init.body === 'string' ? JSON.parse(init.body) : init.body })
    const entry = routes[String(url)]
    if (entry === undefined) return new Response('not found', { status: 404 })
    const value = typeof entry === 'function' ? await entry(String(url), init) : entry
    if (value instanceof Response) return value
    return new Response(JSON.stringify(value), { status: 200, headers: { 'content-type': 'application/json' } })
  }
  resetBootstrapDataCache()
  return () => { globalThis.fetch = original; resetBootstrapDataCache() }
}

const PATHS = [...GUIDE_DEVICES.map((d) => d.client)]

test('the recommended clients are the ones of the resource list and the download page', () => {
  const known = new Set()
  for (const r of CLIENT_RESOURCES) { if (r.href) known.add(r.href); if (r.zhHref) known.add(r.zhHref) }
  known.add(HARMONY_CLIENT_URL)
  for (const p of CLIENT_PLATFORMS) { for (const key of ['project', 'release']) if (p[key]) known.add(p[key]); const urls = p.getDownloadUrls?.('en'); if (urls?.primary) known.add(urls.primary) }
  for (const id of new Set(PATHS)) {
    const client = GUIDE_CLIENTS[id]
    for (const url of [client.url, client.zhUrl, client.project].filter(Boolean)) assert.ok(known.has(url), `${id}: ${url} is not a link the Web UI already uses`)
  }
})

test('the QR path draws exactly the modules of the qrcode package', () => {
  for (const url of ['https://github.com/qiin2333/moonlight-vplus', 'https://apps.apple.com/cn/app/voidlink/id6747717070']) {
    const modules = QRCode.create(url, { errorCorrectionLevel: 'M' }).modules
    const { size, d } = qrPath(modules, 2)
    assert.equal(size, modules.size + 4)
    const painted = new Set()
    for (const [, x, y, w] of d.matchAll(/M(\d+) (\d+)h(\d+)v1h-\d+z/g)) for (let i = 0; i < +w; i += 1) painted.add(`${+x + i},${y}`)
    let dark = 0
    for (let r = 0; r < modules.size; r += 1) for (let c = 0; c < modules.size; c += 1) { const on = !!modules.get(r, c); if (on) dark += 1; assert.equal(painted.has(`${c + 2},${r + 2}`), on, `${r},${c}`) }
    assert.equal(painted.size, dark)
  }
})

test('first run opens the guide: setup not completed, nobody paired, not finished', () => {
  const config = { setup_wizard_completed: 'false' }
  const clients = { status: true, named_certs: [] }
  assert.equal(decideAutoOpen({ config, clients }), true)
  assert.equal(decideAutoOpen({ config: { setup_wizard_completed: true }, clients }), false)
  assert.equal(decideAutoOpen({ config, clients: { status: true, named_certs: [{ name: 'Pixel', uuid: '1' }] } }), false)
  assert.equal(decideAutoOpen({ config, clients, progress: { finished: true } }), false)
  assert.equal(decideAutoOpen({ config, clients, embedded: true }), false)
  assert.equal(decideAutoOpen({ config, clients, suppressed: true }), false)
  assert.equal(decideAutoOpen({ config, clients, dismissed: true }), false)
})

test('a host that answers badly never opens the guide', () => {
  const config = { setup_wizard_completed: false }
  for (const clients of [null, undefined, 'x', {}, { named_certs: 'x' }, { status: false, named_certs: [] }, []]) assert.equal(decideAutoOpen({ config, clients }), false, JSON.stringify(clients))
  assert.equal(decideAutoOpen({ clients: { named_certs: [] } }), false)
  assert.equal(decideAutoOpen({}), false)
})

test('the progress of the Web UI is kept under its own key', () => {
  const values = new Map()
  const storage = { getItem: (k) => values.get(k) ?? null, setItem: (k, v) => values.set(k, v), removeItem: (k) => values.delete(k) }
  writeProgress(storage, 'foundation.guide.web', { step: 'install', device: 'ios', visited: ['setup', 'install'] })
  assert.equal(readProgress(storage, 'foundation.guide.web').device, 'ios')
  assert.equal(readProgress(storage, 'foundation.guide.panel').step, null)
})

test('credentials: the host answer decides, a network failure has its own message', async () => {
  const restore = host({ '/api/password': () => ({ status: 'true' }) })
  assert.deepEqual(await postCredentials({ newUsername: 'a', newPassword: 'b', confirmNewPassword: 'b' }), { ok: true })
  restore()
  const refused = host({ '/api/password': { status: 'false', error: 'Password too weak' } })
  assert.deepEqual(await postCredentials({}), { ok: false, error: 'Password too weak' })
  refused()
  const empty = host({ '/api/password': { status: false } })
  assert.deepEqual(await postCredentials({}), { ok: false, error: 'welcome.server_error' })
  empty()
  const original = globalThis.fetch
  globalThis.fetch = async () => { throw new Error('offline') }
  const logged = console.error
  console.error = () => {}
  assert.deepEqual(await postCredentials({}), { ok: false, error: 'welcome.network_error' })
  console.error = logged
  globalThis.fetch = original
})

test('立即跳转 leaves at once and the redirect that was waiting does not come twice', async () => {
  const restore = host({ '/api/password': { status: true } }, [])
  const moved = []
  const result = await createAccountCallback((key) => key, { redirectMs: 60, navigate: (to) => moved.push(to) })({ username: 'sunshine', password: 'pw', confirm: 'pw' })
  result.now()
  assert.deepEqual(moved, ['/'])
  await new Promise((resolve) => setTimeout(resolve, 120))
  assert.deepEqual(moved, ['/'], 'the timer is cleared')
  restore()
})

test('the account step leaves for the sign-in after a good answer', async () => {
  const calls = []
  const restore = host({ '/api/password': { status: true } }, calls)
  const moved = []
  const create = createAccountCallback((key) => key, { redirectMs: 0, navigate: (to) => moved.push(to) })
  const result = await create({ username: 'sunshine', password: 'pw', confirm: 'pw' })
  await new Promise((resolve) => setTimeout(resolve, 5))
  assert.equal(result.ok, true)
  assert.equal(result.leave, true)
  assert.equal(typeof result.now, 'function', 'the guide offers 立即跳转')
  assert.deepEqual(moved, ['/'])
  assert.deepEqual(calls[0].body, { newUsername: 'sunshine', newPassword: 'pw', confirmNewPassword: 'pw' })
  restore()
  const bad = host({ '/api/password': { status: false, error: 'welcome.password_mismatch' } })
  const failed = await createAccountCallback((key) => `T:${key}`, { redirectMs: 0, navigate: () => assert.fail('must not leave') })({ username: 'a', password: 'b', confirm: 'c' })
  assert.deepEqual(failed, { ok: false, error: 'T:welcome.error T:welcome.password_mismatch' })
  bad()
})

test('setup choices keep the whole configuration and change only what was chosen', async () => {
  const calls = []
  const restore = host({ '/api/config': (url, init) => (init.method === 'POST' ? { status: true } : { locale: 'de', port: 47989, output_name: 'ZakoHDR', setup_wizard_completed: false, adapter_name: 'Old' }) }, calls)
  const kept = await saveSetupChoices({ adapter: 'RTX 4080', display: '\\\\.\\DISPLAY1', prep: 'ensure_only_display' })
  assert.equal(kept.response.ok, true)
  assert.deepEqual(calls[1].body, { locale: 'de', port: 47989, output_name: '\\\\.\\DISPLAY1', setup_wizard_completed: true, adapter_name: 'RTX 4080', display_device_prep: 'ensure_only_display' })
  await markSetupCompleted()
  assert.deepEqual(calls[3].body, { locale: 'de', port: 47989, output_name: 'ZakoHDR', setup_wizard_completed: true, adapter_name: 'Old' })
  await saveSetupChoices({ locale: 'zh' })
  assert.equal(calls[5].body.locale, 'zh')
  restore()
})

test('the host config fills the setup step', async () => {
  const restore = host({ '/api/config': { platform: 'windows', locale: 'en', setup_wizard_completed: 'false', adapters: [{ name: 'RTX 4080' }], display_devices: [{ device_id: 'D1', data: 'FRIENDLY NAME: F32' }], adapter_name: '', output_name: '', pair_name: 'My phone' } })
  const { state, callbacks } = useGuide()
  await callbacks.loadSetup()
  assert.equal(state.setup.completed, false)
  assert.equal(state.setup.virtual, true)
  assert.equal(state.setup.adapters.length, 1)
  assert.equal(state.setup.displayDevices.length, 1)
  assert.equal(state.pair.name, 'My phone')
  assert.equal(state.host.platform, 'windows')
  restore()
  const linux = host({ '/api/config': { platform: 'linux', setup_wizard_completed: true, locale: 'zh' } })
  const other = useGuide()
  await other.callbacks.loadSetup()
  assert.equal(other.state.setup.virtual, false)
  assert.equal(other.state.setup.completed, true)
  assert.equal(other.state.setup.showLanguage, true)
  linux()
})

test('addresses, clients and sessions: real answers fill the state, bad ones leave it empty', async () => {
  const restore = host({
    '/api/network/listeners': { status: true, addresses: [{ name: 'eth0', address: '192.0.2.20' }] },
    '/api/clients/list': { status: true, named_certs: [{ name: 'Pixel 9', uuid: 'u1', enabled: true }] },
    '/api/runtime/sessions': { success: true, sessions: [{ client_name: 'Pixel 9', app_name: 'Desktop', width: 1920, height: 1080, fps: 60 }] },
  })
  const { state, callbacks } = useGuide()
  await Promise.all([callbacks.refreshAddresses(), callbacks.refreshClients(), callbacks.refreshSessions()])
  assert.equal(state.host.addresses[0].address, '192.0.2.20')
  assert.equal(state.clients[0].name, 'Pixel 9')
  assert.equal(state.sessions[0].app_name, 'Desktop')
  restore()

  const broken = host({
    '/api/network/listeners': { status: false, error: 'x' },
    '/api/clients/list': { named_certs: 'nope' },
    '/api/runtime/sessions': () => new Response(JSON.stringify({ error: 'local only' }), { status: 403 }),
  })
  const bad = useGuide()
  await Promise.all([bad.callbacks.refreshAddresses(), bad.callbacks.refreshClients(), bad.callbacks.refreshSessions()])
  assert.equal(bad.state.host.addresses, null)
  assert.equal(bad.state.clients, null)
  assert.equal(bad.state.sessions, null)
  broken()

  const html = host({ '/api/clients/list': () => new Response('<html>login</html>', { status: 200 }), '/api/runtime/sessions': () => new Response('<html>', { status: 200 }) })
  const page = useGuide()
  await page.callbacks.refreshClients()
  await page.callbacks.refreshSessions()
  assert.equal(page.state.clients, null)
  assert.equal(page.state.sessions, null)
  html()
})

test('pairing a client: the PIN goes to the host with the name, the client list shows the new device', async () => {
  const clients = [{ name: 'Old', uuid: 'a' }]
  const calls = []
  const restore = host({
    '/api/pin': (url, init) => { const body = JSON.parse(init.body); if (body.pin === '0000') return { status: false, error: 'Wrong PIN' }; clients.push({ name: body.name, uuid: 'b' }); return { status: true } },
    '/api/clients/list': () => ({ status: true, named_certs: clients }),
  }, calls)
  const { state, callbacks } = useGuide()
  await callbacks.refreshClients()
  const before = state.clients.map((c) => c.uuid)
  assert.deepEqual(await callbacks.pair('0000', 'Pixel'), { ok: false, error: 'Wrong PIN' })
  assert.deepEqual(await callbacks.pair('1234', 'Pixel'), { ok: true })
  await callbacks.refreshClients()
  const added = state.clients.filter((c) => !before.includes(c.uuid))
  assert.deepEqual(added.map((c) => c.name), ['Pixel'])
  assert.deepEqual(calls.find((c) => c.url === '/api/pin' && c.body.pin === '1234').body, { pin: '1234', name: 'Pixel' })
  restore()
})

test('restoring the built-in apps reuses the apps page rules and saves once', async () => {
  const calls = []
  let stored = [{ name: 'Steam Big Picture', cmd: 'x' }]
  const restore = host({
    '/api/apps': (url, init) => { if (init.method === 'POST') { stored = JSON.parse(init.body).apps; return { status: true } } return { apps: stored } },
    '/api/config': { platform: 'windows' },
  }, calls)
  const { callbacks } = useGuide()
  const first = await callbacks.restoreApps()
  assert.deepEqual(first, { ok: true, result: 'done' })
  assert.ok(stored.some((app) => /desktop/i.test(app.name)))
  assert.equal(calls.filter((c) => c.method === 'POST').length, 1)
  assert.deepEqual(await callbacks.restoreApps(), { ok: true, result: 'none' })
  assert.equal(calls.filter((c) => c.method === 'POST').length, 1)
  restore()
  const failing = host({ '/api/apps': () => new Response('{}', { status: 500 }), '/api/config': { platform: 'windows' } })
  const logged = console.error
  console.error = () => {}
  const result = await useGuide().callbacks.restoreApps()
  console.error = logged
  assert.equal(result.ok, false)
  failing()
})

test('an upgrade shows the guide once per version of the host, with what is done left out', () => {
  const config = { setup_wizard_completed: true, version: '2.4.0' }
  const clients = { status: true, named_certs: [{ name: 'Pixel', uuid: '1' }] }
  const entry = decideGuideEntry({ config, clients, progress: null, version: '2.4.0' })
  assert.equal(entry.mode, 'upgrade')
  assert.deepEqual(entry.skip, ['account', 'setup', 'hostdone', 'install', 'address', 'pair'])
  // no client paired: the client steps stay
  assert.deepEqual(decideGuideEntry({ config, clients: { status: true, named_certs: [] }, progress: null, version: '2.4.0' }).skip, ['account', 'setup'])
  // seen for this version (closed, skipped or finished): not again; a newer version opens it again
  const seen = { finished: false, visited: ['whatsnew'], version: '2.4.0', mode: 'upgrade' }
  assert.equal(decideGuideEntry({ config, clients, progress: seen, version: '2.4.0' }), null)
  assert.equal(decideGuideEntry({ config, clients, progress: seen, version: '2.5.0' }).mode, 'upgrade')
  // the page that is embedded in the panel never opens it (the panel has its own)
  assert.equal(decideGuideEntry({ config, clients, progress: null, version: '2.4.0', embedded: true }), null)
  // a first install is the whole flow
  assert.equal(decideGuideEntry({ config: { setup_wizard_completed: false, version: '2.4.0' }, clients: { status: true, named_certs: [] }, progress: null, version: '2.4.0' }).mode, 'fresh')
})

test('the guide scans the game libraries and adds the chosen games without leaving the page', async () => {
  const calls = []
  const restore = host({
    '/api/apps/scan': () => ({ games: [{ name: 'Portal', platform: 'steam', launch: 'steam://run/400', install_dir: 'C:/Steam/Portal', id: '400' }] }),
    '/api/apps': (url, init) => (init.method === 'POST' ? { status: true } : { apps: [{ name: 'Desktop' }] }),
  }, calls)
  const { callbacks } = useGuide()
  const scan = await callbacks.scanGames(['steam'])
  assert.equal(scan.ok, true)
  assert.equal(scan.games[0].name, 'Portal')
  assert.ok(scan.games[0].key)
  assert.deepEqual(calls.find((c) => c.url.endsWith('/api/apps/scan')).body, { sources: ['steam'] })
  const added = await callbacks.addGames(scan.games)
  assert.deepEqual(added, { ok: true, added: 1 })
  const saved = calls.filter((c) => c.method === 'POST' && c.url.endsWith('/api/apps')).at(-1).body.apps
  assert.deepEqual(saved.map((a) => a.name), ['Desktop', 'Portal'])
  assert.equal(saved[1].cmd, 'steam://run/400')
  assert.equal(typeof callbacks.openTools, 'function')
  restore()
})

test('a host that cannot scan says so', async () => {
  const restore = host({ '/api/apps/scan': () => new Response('no', { status: 404 }) })
  const { callbacks } = useGuide()
  const scan = await callbacks.scanGames(['steam'])
  assert.equal(scan.ok, false)
  assert.equal(scan.unsupported, true)
  restore()
})

test('the address step reads the port the host runs, the one it saved and the name clients list; setPort saves only the port', async () => {
  const calls = []
  const restore = host({
    '/api/network/listeners': { status: true, active: { port: 47989 }, saved: { port: 48989 }, addresses: [{ name: 'eth0', address: '192.0.2.20' }] },
    '/api/config': (url, init) => (init.method === 'POST' ? { status: true } : { sunshine_name: 'SKY-PC', port: 47989, upnp: 'enabled', external_endpoints: '' }),
  }, calls)
  const { state, callbacks } = useGuide()
  await callbacks.loadNetwork()
  assert.deepEqual([state.network.port, state.network.activePort, state.host.name], [48989, 47989, 'SKY-PC'])
  assert.deepEqual(await callbacks.setPort(49989), { ok: true })
  const post = calls.find((c) => c.method === 'POST')
  assert.equal(post.body.port, 49989); assert.equal(post.body.sunshine_name, 'SKY-PC'); assert.equal(post.body.upnp, 'enabled')
  assert.equal(state.network.port, 49989)
  restore()
  const refused = host({ '/api/config': (url, init) => (init.method === 'POST' ? new Response('port out of range', { status: 400 }) : {}) })
  const { callbacks: other } = useGuide()
  assert.deepEqual(await other.setPort(80), { ok: false, error: 'port out of range' })
  refused()
})

test('a second device does not open the guide while the first runs it, and a guide closed anywhere stays closed (the host keeps both)', () => {
  const clients = { status: true, named_certs: [] }
  const now = 1_800_000_000_000
  const base = { clients, version: '1.3.9', now, device: 'pc' }
  const running = { setup_wizard_completed: false, guide_done: 'intro,account', guide_active: `phone@${now / 1000 - 20}` }
  assert.equal(decideGuideEntry({ ...base, config: running }), null, 'the phone took a step 20 seconds ago')
  assert.deepEqual(decideGuideEntry({ ...base, config: { ...running, guide_active: `pc@${now / 1000 - 20}` } }), { mode: 'fresh', skip: [] }, 'this device is the one running it')
  assert.equal(decideGuideEntry({ ...base, config: { setup_wizard_completed: false, guide_closed: '1.3.9' } }), null, 'closed on the phone')
  assert.deepEqual(decideGuideEntry({ ...base, config: { setup_wizard_completed: false, guide_closed: '1.3.8' } }), { mode: 'fresh', skip: [] }, 'closed for an older version')
})

test('the port check of the address step is the host 体检 (network group), asked through the Web UI request and worded in the language of the guide', async () => {
  const { readFileSync } = await import('node:fs')
  const { apiFetch } = await import('../utils/apiFetch.js')
  const { checkPortsViaDoctor } = await import('foundation-ui/vue/guide.js')
  const source = readFileSync(new URL('../composables/useGuide.js', import.meta.url), 'utf8')
  assert.match(source, /checkPorts: async \(\) => \{/, 'in the shared callbacks: the guide window and the welcome page both have it')
  assert.match(source, /\/\^zh\/i\.test\(\(await import\('\.\.\/config\/i18n\.js'\)\)\.activeLocale\.value \|\| ''\) \? 'zh' : 'en'/)
  const calls = []
  const report = { version: 1, checked_at: 'x', groups: [{ id: 'network', title: 'Network', items: ['network.ports'] }], items: [{ id: 'network.ports', group: 'network', status: 'warn', title: '端口', detail: 'UDP 47998' }] }
  host({ '/api/doctor?only=network&lang=zh': report }, calls)
  const r = await checkPortsViaDoctor(async (path) => { const response = await apiFetch(path); return { ok: response.ok, status: response.status, json: await response.json().catch(() => null) } }, 'zh')
  assert.equal(r.ok, true)
  assert.deepEqual(r.items.map((i) => i.id), ['network.ports'])
  assert.equal(r.worst, 'warn')
  assert.deepEqual(calls.map((c) => c.url), ['/api/doctor?only=network&lang=zh'])
})
