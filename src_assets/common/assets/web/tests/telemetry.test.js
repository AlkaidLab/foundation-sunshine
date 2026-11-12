import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const read = (p) => fs.readFileSync(path.join(here, '..', p), 'utf8')

// What the Web UI sends goes to the host it is served from (POST /api/telemetry/events); no script of a third party is loaded (docs/privacy_telemetry.md).
const withBrowser = async (run, { stored = {} } = {}) => {
  const store = new Map(Object.entries(stored))
  const calls = []
  const original = { localStorage: globalThis.localStorage, sessionStorage: globalThis.sessionStorage, fetch: globalThis.fetch, raf: globalThis.requestAnimationFrame }
  globalThis.localStorage = { getItem: (k) => (store.has(k) ? store.get(k) : null), setItem: (k, v) => store.set(k, String(v)) }
  globalThis.sessionStorage = { getItem: () => null, setItem: () => {} }
  globalThis.requestAnimationFrame = (fn) => setTimeout(fn, 0)
  globalThis.fetch = async (url, options) => { calls.push({ url, options, body: JSON.parse(options.body) }); return { ok: true, status: 200, json: async () => ({ accepted: 1 }) } }
  try { await run(calls) } finally {
    for (const [k, v] of Object.entries({ localStorage: original.localStorage, sessionStorage: original.sessionStorage, fetch: original.fetch, requestAnimationFrame: original.raf })) { if (v === undefined) delete globalThis[k]; else globalThis[k] = v }
  }
}
const mod = async () => import(`../config/telemetry.js?${Math.random()}`)

test('events go to the host in the contract shape, never to a Google host', async () => {
  await withBrowser(async (calls) => {
    const { trackEvents, flushTelemetry, setTelemetryContext } = await mod()
    setTelemetryContext({ version: '5.2.0.1112.杂鱼' })
    trackEvents.appStart()
    trackEvents.userAction('apps_saved', { count: 3, name: 'My Secret Game' })
    trackEvents.errorOccurred('config_save', 'failed at C:\\Users\\Alice\\x.json for 192.0.2.5')
    trackEvents.pageView('display-modes')
    await new Promise((r) => setTimeout(r, 20))
    await flushTelemetry()
    assert.equal(calls.length, 1)
    assert.equal(calls[0].url, '/api/telemetry/events')
    assert.equal(calls[0].options.method, 'POST')
    const { product, version, events } = calls[0].body
    assert.equal(product, 'web'); assert.equal(version, '5.2.0.1112.杂鱼')
    const by = (name) => events.filter((e) => e.name === name)
    assert.equal(by('app_start')[0].kind, 'usage'); assert.equal(by('app_start')[0].props.cold, true); assert.ok(Number.isInteger(by('app_start')[0].props.start_ms))
    assert.deepEqual(by('feature_used')[0].props, { feature: 'web.apps_saved' }, 'a fixed word, no count, no name')
    const error = by('error')[0]
    assert.equal(error.kind, 'error'); assert.equal(error.props.component, 'web'); assert.equal(error.props.code, 'config_save')
    assert.equal('message' in error.props, false, 'an error event has no text')
    assert.doesNotMatch(JSON.stringify(calls[0].body), /Alice|192\.168/)
    const page = by('panel_page')[0]
    assert.equal(page.kind, 'experience'); assert.equal(page.props.page, 'display-modes'); assert.ok(Number.isInteger(page.props.time_to_content_ms))
    assert.doesNotMatch(JSON.stringify(calls[0].body), /Secret Game|gtag|firebase|google/i)
  })
})

test('usage statistics off: no usage or experience event is queued, an error still is; error reports off: the other way round', async () => {
  await withBrowser(async (calls) => {
    const { trackEvents, flushTelemetry } = await mod()
    trackEvents.userAction('x'); trackEvents.appStart(); trackEvents.pageView('home')
    trackEvents.errorOccurred('boom', 'text')
    await new Promise((r) => setTimeout(r, 20))
    await flushTelemetry()
    assert.deepEqual(calls[0].body.events.map((e) => e.name), ['error'])
  }, { stored: { 'foundation-ui.telemetry.consent': 'off', 'foundation-ui.telemetry.errors': 'on' } })
  await withBrowser(async (calls) => {
    const { trackEvents, flushTelemetry } = await mod()
    trackEvents.errorOccurred('boom', 'text'); trackEvents.userAction('y')
    await flushTelemetry()
    assert.deepEqual(calls[0].body.events.map((e) => e.name), ['feature_used'])
  }, { stored: { 'foundation-ui.telemetry.errors': 'off' } })
  await withBrowser(async (calls) => {
    const { trackEvents, flushTelemetry } = await mod()
    trackEvents.errorOccurred('boom', 'text'); trackEvents.userAction('y'); trackEvents.pageView('home')
    await new Promise((r) => setTimeout(r, 20))
    await flushTelemetry()
    assert.equal(calls.length, 0, 'both off: nothing is requested')
  }, { stored: { 'foundation-ui.telemetry.errors': 'off', 'foundation-ui.telemetry.consent': 'off' } })
})

test('an embedded page (inside the control panel, which counts for itself) sends nothing', async () => {
  await withBrowser(async (calls) => {
    const { trackEvents, flushTelemetry, setTelemetryContext } = await mod()
    setTelemetryContext({ version: '1', embedded: true })
    trackEvents.appStart(); trackEvents.userAction('x'); trackEvents.errorOccurred('a', 'b')
    await flushTelemetry()
    assert.equal(calls.length, 0)
  })
})

test('a turned-off choice drops what was waiting', async () => {
  await withBrowser(async (calls) => {
    const { trackEvents, flushTelemetry } = await mod()
    globalThis.window = new EventTarget()
    try {
      const again = await import(`../config/telemetry.js?${Math.random()}`)
      again.trackEvents.userAction('queued_before_off')
      globalThis.localStorage.setItem('foundation-ui.telemetry.consent', 'off')
      window.dispatchEvent(Object.assign(new Event('foundation-telemetry-consent'), { detail: 'off' }))
      await again.flushTelemetry()
      assert.equal(calls.length, 0)
    } finally { delete globalThis.window }
    void trackEvents; void flushTelemetry
  })
})

test('no Google script, SDK or measurement id is left in the Web UI', () => {
  const pkg = JSON.parse(fs.readFileSync(path.join(here, '../../../../../package.json'), 'utf8'))
  assert.ok(!pkg.dependencies?.firebase && !pkg.devDependencies?.firebase, 'the firebase package is not a dependency')
  assert.ok(!fs.existsSync(path.join(here, '../config/firebase.js')))
  for (const f of ['config/telemetry.js', 'views/WebShell.vue', 'views/Home.vue', 'composables/useConfig.js', 'composables/useApps.js', 'composables/useVersion.js']) {
    assert.doesNotMatch(read(f), /firebase|gtag|googletagmanager|google-analytics|G-F20721ZDL1/i, f)
  }
})

test('one page view, sent by the shell: the pages do not send their own', () => {
  for (const f of ['views/Home.vue', 'views/Config.vue', 'views/Apps.vue']) assert.ok(!/trackEvents\.pageView/.test(read(f)), f)
  assert.match(read('views/WebShell.vue'), /trackEvents\.pageView\(String\(page\)\)/)
})

test('the shell tells the sender its version and that it is embedded, and starts counting once', () => {
  const shell = read('views/WebShell.vue')
  assert.match(shell, /setTelemetryContext\(\{ version: v, embedded \}\)/)
  assert.match(shell, /trackEvents\.appStart\(\)/)
  assert.match(shell, /from '\.\.\/config\/telemetry\.js'/)
})

test('no page asks about usage statistics: the shell reads the choice from the host, the welcome page has no question, and 关于 › 隐私 changes it', () => {
  const shell = read('views/WebShell.vue')
  assert.match(shell, /useTelemetryConsent/); assert.doesNotMatch(shell, /MTelemetryConsent|askWhenQuiet|consentOpen/); assert.match(shell, /await telemetry\.refresh\(\) \}\)/)
  const welcome = read('views/Welcome.vue')
  assert.doesNotMatch(welcome, /useTelemetryConsent|setConsent|consentShow|consent:/); assert.match(welcome, /buildGuideSteps\(\{ skip: \['update'\] \}\)/)
  assert.match(read('views/About.vue'), /:consent="telemetry \? telemetry\.consent\.value : ''"/)
})
