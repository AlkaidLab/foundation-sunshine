import assert from 'node:assert/strict'
import { existsSync, readFileSync, writeFileSync } from 'node:fs'
import test, { after, before } from 'node:test'
import { startHarness } from './helpers/editorHarness.js'
import { findAll, textOf } from './helpers/fakeDom.js'
import { COPY } from '../utils/sparxie/copy.js'

// 保存 must not be lit by merely opening an application. Every application of an apps.json captured from a real host (fixtures/sparxie-captured.json is made from those files by the
// host's own gtest, tests/sparxie/test_captured_gate.cpp: host start, then the GET answers the editor reads) is opened in the real editor page, mounted on a small DOM (helpers/editorHarness.js):
//   open: 保存 is off and nothing was written; the page says the input the application really has (a direct launch is keyboard, mouse and gamepad), never an unexplained 直接启动;
//   change one field: 保存 is on; change it back: off; press 保存 while it is off: no request;
//   change and save: the body goes to the host, 保存 is off again, and reopening the application as the host stored it (the host's own answer to that body) is clean again.
// The two variants are a host with a Sparxie that has the touch-ui-switch capability (the patched build) and with one that has not.
// Regenerate the host answers: see tests/sparxie/test_captured_gate.cpp. The bodies the editor sends are kept in fixtures/sparxie-captured-bodies.json (SPARXIE_GATE_WRITE_BODIES=1 rewrites it).
const fx = JSON.parse(readFileSync(new URL('./fixtures/sparxie-captured.json', import.meta.url), 'utf8'))
const bodiesUrl = new URL('./fixtures/sparxie-captured-bodies.json', import.meta.url)
const committedBodies = existsSync(bodiesUrl) ? JSON.parse(readFileSync(bodiesUrl, 'utf8')) : null
const written = { withSwitch: {}, withoutSwitch: {} }
const rows = []
const ZH = COPY.zh

let h
let SparxiePage
let AppPage
let sparxie
let clientOf
before(async () => {
  h = await startHarness()
  SparxiePage = (await h.load('/components/sparxie/SparxieNewApp.vue')).default
  AppPage = (await h.load('/components/AppEditor.vue')).default
  sparxie = await h.load('/composables/useSparxie.js')
  clientOf = (await h.load('/services/sparxieClient.js')).createClient
})
after(async () => {
  await h?.close()
  setTimeout(() => process.exit(process.exitCode ?? 0), 800).unref() // timers of a page that was left mounted by a failed test must not keep the process
  if (process.env.SPARXIE_GATE_WRITE_BODIES) writeFileSync(bodiesUrl, JSON.stringify(written, null, 2) + '\n')
  console.log(`\n${['variant', 'app', 'input', 'open', 'change', 'undo', 'save', 'reopen'].join(' | ')}\n${rows.map((r) => r.join(' | ')).join('\n')}`)
  if (process.env.SPARXIE_GATE_TABLE) writeFileSync(process.env.SPARXIE_GATE_TABLE, `${rows.map((r) => r.join(' | ')).join('\n')}\n`)
})

// the host as the editor sees it: the answers of the host's own code, and a recorded list of what was asked
function hostOf(variant, { saved = null } = {}) {
  const v = fx[variant]
  const calls = []
  const answer = (method, url, body) => {
    const u = new URL(url, 'http://host')
    calls.push({ method, path: u.pathname, body })
    if (u.pathname === '/api/sparxie/status') return v.status
    if (u.pathname === '/api/sparxie/games') return v.games
    if (u.pathname === '/api/sparxie/game-settings') return v.gameSettings[u.searchParams.get('install')] || { ok: true, values: {}, unavailable: [], running: false }
    if (u.pathname === '/api/sparxie/art/refresh') return { ok: true, updated: 0 }
    if (method === 'PUT' && /^\/api\/sparxie\/apps\/\d+$/.test(u.pathname) && saved) return saved.result
    const error = new Error(`not answered: ${method} ${u.pathname}`)
    error.status = 404
    throw error
  }
  const client = clientOf({ get: async (u) => answer('GET', u), post: async (u, b) => answer('POST', u, b), put: async (u, b) => answer('PUT', u, b) })
  return { calls, client, writes: () => calls.filter((c) => c.method !== 'GET' && !/\/art\/refresh$/.test(c.path)) }
}

const inputOf = (app) => app.sparxie.options?.input || (app.sparxie.mode === 'pc' ? 'kbm' : 'touch')
const INPUT_LABEL = { touch: ZH.inputTouch, kbm: ZH.inputKbm }
const flip = (input) => (input === 'touch' ? 'kbm' : 'touch')
const yes = (ok) => (ok ? 'ok' : 'FAIL')

async function openSparxie(variant, app, index, { saved = null } = {}) {
  const host = hostOf(variant, { saved })
  sparxie.setSparxieClient(host.client)
  // a page that opens asks the host for the status again
  const shared = sparxie.useSparxie().state
  shared.loaded = false
  shared.installed = false
  const events = []
  const page = await h.mount(SparxiePage, { edit: { ...app, index }, backLabel: 'x' }, { saved: (a) => events.push(['saved', a]), close: () => events.push(['close']) })
  return { page, host, events }
}

// a later segment may carry the same word (the graphics API of a title also has 自动): the input's radios are the ones of the group named 输入方式
const inputRadio = (page, k) => page.radios(INPUT_LABEL[k]).filter((n) => n.parentNode?.attrs?.role === 'radiogroup' && n.parentNode.attrs['aria-label'] === ZH.inputName)
const selectedInput = (page) => ['touch', 'kbm'].filter((k) => inputRadio(page, k).some((n) => page.selected(n)))

for (const variant of process.env.SPARXIE_GATE_VARIANT ? [process.env.SPARXIE_GATE_VARIANT] : ['withSwitch', 'withoutSwitch']) {
  const apps = fx[variant].apps
  apps.forEach((app, index) => {
    if (!app.sparxie) return
    test(`${variant}: ${app.name} opens clean, changes and undoes, saves and reopens clean`, async () => {
      const row = [variant, app.name, inputOf(app)]
      // ---- open ----
      const first = await openSparxie(variant, app, index)
      const { page, host } = first
      const open = page.saveDisabled() && host.writes().length === 0
      assert.ok(page.saveDisabled(), `${app.name}: 保存 is off when the page opens`)
      assert.deepEqual(selectedInput(page), [inputOf(app)], `${app.name}: the page says the input the application really has`)
      const bar = textOf(findAll(page.root, (n) => n.attrs.role === 'toolbar')[0])
      assert.ok(!bar.includes('直接启动'), `${app.name}: the bar never says 直接启动 for a game of Sparxie: ${bar}`)
      const wantsLine = !fx[variant].status.touchUiSwitch && inputOf(app) === 'kbm' && ['genshin', 'starrail', 'endfield'].includes(app.sparxie.game)
      assert.equal(page.text().includes(ZH.kbmNeedsTouch), wantsLine, `${app.name}: the one line about a Sparxie that cannot keep the PC interface is there when it applies and only then`)
      // pressing 保存 while it is off asks nothing of the host
      await page.press(page.button('保存'))
      assert.equal(host.writes().length, 0, `${app.name}: nothing is written when nothing changed`)

      // ---- change one field, undo it ----
      const other = flip(inputOf(app))
      await page.press(inputRadio(page, other)[0])
      const changed = !page.saveDisabled()
      assert.ok(changed, `${app.name}: changing the input lights 保存`)
      await page.press(inputRadio(page, inputOf(app))[0])
      const undone = page.saveDisabled()
      assert.ok(undone, `${app.name}: changing it back puts 保存 off again`)

      // ---- change and save ----
      await page.press(inputRadio(page, other)[0])
      await page.press(page.button('保存'))
      const puts = host.writes().filter((c) => c.method === 'PUT')
      assert.equal(puts.length, 1, `${app.name}: one save is sent`)
      const body = puts[0].body
      written[variant][app.name] = { change: `input ${inputOf(app)} -> ${other}`, body }
      if (committedBodies && !process.env.SPARXIE_GATE_WRITE_BODIES) assert.deepEqual(body, committedBodies[variant]?.[app.name]?.body, `${app.name}: the editor still sends the body the host's answer was made for (rewrite fixtures/sparxie-captured-bodies.json and the host fixture when the page changes)`)
      page.unmount()

      // ---- the host's answer to that body: the page after the save is off, and the application reopens clean ----
      const after = fx[variant].afterSave?.[app.name]
      if (after) {
        const second = await openSparxie(variant, app, index, { saved: after })
        await second.page.press(inputRadio(second.page, other)[0])
        await second.page.press(second.page.button('保存'))
        assert.ok(second.page.saveDisabled(), `${app.name}: 保存 is off again after the save`)
        assert.ok(second.events.some((e) => e[0] === 'saved'), `${app.name}: the page reports the save`)
        second.page.unmount()
        const stored = after.apps[index]
        const reopened = await openSparxie(variant, stored, index)
        assert.ok(reopened.page.saveDisabled(), `${app.name}: reopened as the host stored it, 保存 is off`)
        assert.deepEqual(selectedInput(reopened.page), [other], `${app.name}: and it shows the input that was saved`)
        assert.equal(reopened.host.writes().length, 0)
        row.push(yes(open), yes(changed), yes(undone), 'sent, off after', 'reopen clean')
        reopened.page.unmount()
      } else {
        row.push(yes(open), yes(changed), yes(undone), 'sent (no host answer yet)', '-')
      }
      rows.push(row)
    })
  })

  // The game settings follow the game: an application holds only the items the person changed. Opening lights nothing (above); changing an item lights 保存 and the item shows 已更改 with a way
  // back; the way back (or 全部跟随游戏) returns the item to following the game, and when that is what the application stored, 保存 is off again.
  const backButtons = (page) => findAll(page.root, (n) => n.tagName === 'button' && String(n.attrs['aria-label'] || '').includes(ZH.gsBack))
  apps.forEach((app, index) => {
    if (!app.sparxie) return
    test(`${variant}: ${app.name} game settings follow the game; a changed item lights 保存 and following again clears it`, async () => {
      const held = Object.keys(app.sparxie.settings || {})
      const { page, host } = await openSparxie(variant, app, index)
      const sw = page.switches(ZH.vsync)[0]
      if (!sw) { page.unmount(); return } // a title without a V-sync item (Endfield, or a title whose settings the host does not manage)
      assert.ok(page.saveDisabled(), `${app.name}: opens clean`)
      assert.equal(backButtons(page).length, held.length, `${app.name}: only the items it holds carry the way back (${held.join(', ') || 'none'})`)
      assert.equal(page.text().includes(ZH.gsAllFollow), true, `${app.name}: 全部跟随游戏 is there`)
      // change one item
      await page.press(sw)
      assert.ok(!page.saveDisabled(), `${app.name}: changing an item lights 保存`)
      assert.ok(backButtons(page).length >= 1, `${app.name}: the changed item can go back to following the game`)
      if (held.length === 0) {
        // the one item goes back to following: nothing is held again, as stored
        await page.press(backButtons(page)[0])
        assert.ok(page.saveDisabled(), `${app.name}: back to following the game, 保存 is off again`)
        // and the save of a held item sends exactly that item
        await page.press(page.switches(ZH.vsync)[0])
        await page.press(page.button('保存'))
        const put = host.writes().filter((c) => c.method === 'PUT')
        assert.equal(put.length, 1, `${app.name}: one save`)
        assert.deepEqual(Object.keys(put[0].body.settings || {}), ['vsync'], `${app.name}: only the changed item is sent`)
        assert.equal('settingsMode' in put[0].body, false, `${app.name}: no settings mode any more`)
      } else {
        // an application that holds items: 全部跟随游戏 differs from what it stored, so 保存 stays on, and saving it sends no items
        await page.press(page.button(ZH.gsAllFollow))
        assert.ok(!page.saveDisabled(), `${app.name}: dropping what it held is a change`)
        assert.equal(backButtons(page).length, 0, `${app.name}: nothing is held any more`)
        await page.press(page.button('保存'))
        const put = host.writes().filter((c) => c.method === 'PUT')
        assert.equal(put.length, 1, `${app.name}: one save`)
        assert.deepEqual(put[0].body.settings, {}, `${app.name}: every item follows the game again`)
      }
      page.unmount()
    })
  })

  // the applications that are not Sparxie's: the generic editor of the apps page
  apps.forEach((app, index) => {
    if (app.sparxie) return
    test(`${variant}: ${app.name} (the generic application editor) opens clean, changes and undoes`, async () => {
      const host = hostOf(variant)
      sparxie.setSparxieClient(host.client)
      const saves = []
      const page = await h.mount(AppPage, { app: { ...app, index }, platform: 'windows' }, { 'save-app': (a) => saves.push(a), close: () => {} })
      assert.ok(page.saveDisabled('_common.save'), `${app.name}: 保存 is off when the page opens`)
      await page.press(page.button('_common.save'))
      assert.equal(saves.length, 0, `${app.name}: nothing is saved when nothing changed`)
      const toggle = page.switches('apps.auto_detach')[0]
      assert.ok(toggle, 'the editor has the switch the gate changes')
      await page.press(toggle)
      assert.ok(!page.saveDisabled('_common.save'), `${app.name}: a change lights 保存`)
      await page.press(page.switches('apps.auto_detach')[0])
      assert.ok(page.saveDisabled('_common.save'), `${app.name}: undone, 保存 is off again`)
      await page.press(page.switches('apps.auto_detach')[0])
      await page.press(page.button('_common.save'))
      assert.equal(saves.length, 1, `${app.name}: the change is saved`)
      page.unmount()
      rows.push([variant, app.name, '(generic)', 'ok', 'ok', 'ok', 'sent', '-'])
    })
  })
}

// ---- Endfield, added through the host's own path: the editor offers what the host describes for it, and does not say "this game version does not allow" ----
for (const variant of process.env.SPARXIE_GATE_VARIANT ? [process.env.SPARXIE_GATE_VARIANT] : ['withSwitch', 'withoutSwitch']) {
  test(`${variant}: Endfield opens clean with its own items and no game-settings list that cannot be changed`, async () => {
    const e = fx[variant].endfield
    assert.ok(e, 'the host dumped an Endfield application')
    // the same host, with the answers of the Endfield dump
    const calls = []
    const v = fx[variant]
    const answer = (method, url, body) => {
      const u = new URL(url, 'http://host')
      calls.push({ method, path: u.pathname, body })
      if (u.pathname === '/api/sparxie/status') return v.status
      if (u.pathname === '/api/sparxie/games') return e.games
      if (u.pathname === '/api/sparxie/game-settings') return e.gameSettings
      if (u.pathname === '/api/sparxie/art/refresh') return { ok: true, updated: 0 }
      throw Object.assign(new Error('not answered'), { status: 404 })
    }
    sparxie.setSparxieClient(clientOf({ get: async (u) => answer('GET', u), post: async (u, b) => answer('POST', u, b), put: async (u, b) => answer('PUT', u, b) }))
    const shared = sparxie.useSparxie().state
    shared.loaded = false
    shared.installed = false
    const page = await h.mount(SparxiePage, { edit: { ...e.app, index: 0 }, backLabel: 'x' }, {})
    const text = page.text()
    if (process.env.SPARXIE_GATE_DEBUG) console.log('ENDFIELD radios', findAll(page.root, (n) => n.attrs.role === 'radio').map((n) => `${n.attrs['aria-label']}=${n.attrs['aria-checked']}`).join(' | '), '\nTEXT', text.slice(0, 1200))
    assert.ok(page.saveDisabled(), 'Endfield: 保存 is off when the page opens')
    // the input is two short names (触控, 键鼠与手柄): segments like the input of every other game, never a menu behind a click; there is no 自动 (an app that had it was migrated to touch)
    assert.deepEqual(['touch', 'kbm'].map((k) => inputRadio(page, k).length), [1, 1], 'Endfield: the two inputs are two segments')
    assert.deepEqual(selectedInput(page), ['touch'], 'Endfield: the input is touch')
    assert.ok(!text.includes('按客户端设备'), 'Endfield: nothing says the input follows the client device')
    // the session's size is the app's 显示 choice (the session size item of Sparxie's own list is not drawn twice)
    assert.ok(page.radios(ZH.displayFollow).some((n) => page.selected(n)) && !page.radios(ZH.displayFixed).some((n) => page.selected(n)), 'the display follows the stream')
    assert.ok(text.includes(ZH.displayFollowLater), 'and the one line says so')
    assert.ok(!text.includes(`${ZH.resolution} ${ZH.resolutionAuto}`), 'there is no second resolution row')
    const row = e.games.catalog.find((c) => c.game === 'endfield')
    const fields = Object.fromEntries(row.sparxie.fields.map((f) => [f.key, f]))
    assert.deepEqual(fields.input.options.map((o) => o.value), ['touch', 'kbm'], 'the input offers touch and keyboard, mouse and gamepad, and no automatic choice'); assert.equal(fields.input.default, 'touch')
    assert.ok(fields.fps.options.some((o) => o.value === 240) && fields.fps.options.every((o) => o.value === 'display' || o.value >= 60), 'the target frame rate goes above 120')
    assert.ok(fields.resolution.options.some((o) => o.value === 'auto') && fields.resolution.default === 'auto')
    assert.ok(text.includes(ZH.fpsCap) && text.includes(ZH.priority), 'the launch options are there')
    assert.ok(!text.includes('这个版本的游戏不支持修改'), `no unavailable list for a title whose settings are not managed: ${text.slice(0, 200)}`)
    assert.ok(text.includes(ZH.gsNotManaged('明日方舟：终末地')), 'one plain line says the in-game settings are not changed')
    assert.equal(page.radios(ZH.gsModePreset).length, 0, 'no game-settings mode to choose')
    // a change and back
    await page.press(page.switches(ZH.bgLimit)[0])
    assert.ok(!page.saveDisabled(), 'changing a launch option lights 保存')
    await page.press(page.switches(ZH.bgLimit)[0])
    assert.ok(page.saveDisabled(), 'and changing it back puts it off')
    // 固定: Endfield offers the size and the window mode like every other Unity title; switching lights 保存, back to 跟随串流 puts it off
    await page.press(page.radios(ZH.displayFixed)[0])
    assert.ok(!page.saveDisabled(), 'Endfield: 固定 lights 保存')
    const fixedText = page.text()
    assert.ok(fixedText.includes(ZH.displayMode) && fixedText.includes(ZH.resolution), `Endfield: a fixed display has a size and a window mode: ${fixedText.slice(0, 500)}`)
    await page.press(page.radios(ZH.displayFollow)[0])
    assert.ok(page.saveDisabled(), 'Endfield: back to 跟随串流, 保存 is off')
    assert.equal(calls.filter((c) => c.method !== 'GET' && !/refresh/.test(c.path)).length, 0)
    page.unmount()
    rows.push([variant, '终末地 (Endfield)', 'touch', 'ok', 'ok', 'ok', '-', '-'])
  })
}

// ---- 显示: the choice of the display (跟随串流 or 固定) on every captured application: it opens as 跟随串流 and clean, a switch to 固定 lights 保存, back puts it off, and a saved fixed display is sent as text ----
for (const variant of process.env.SPARXIE_GATE_VARIANT ? [process.env.SPARXIE_GATE_VARIANT] : ['withSwitch', 'withoutSwitch']) {
  fx[variant].apps.forEach((app, index) => {
    if (!app.sparxie) return
    test(`${variant}: ${app.name} opens following the stream; 固定 lights 保存, 跟随串流 puts it off, and a saved fixed display goes out as text`, async () => {
      const { page, host } = await openSparxie(variant, app, index)
      const follows = () => page.radios(ZH.displayFollow).some((n) => page.selected(n))
      assert.ok(page.saveDisabled() && follows(), `${app.name}: opens clean and following the stream`)
      assert.ok(page.text().includes(ZH.displayFollowLater), `${app.name}: the one line says what the display will be`)
      await page.press(page.radios(ZH.displayFixed)[0])
      assert.ok(!page.saveDisabled(), `${app.name}: 固定 lights 保存`)
      assert.ok(page.text().includes(ZH.resolution) || page.text().includes(ZH.displayMode), `${app.name}: and offers the size or the window mode`)
      await page.press(page.radios(ZH.displayFollow)[0])
      assert.ok(page.saveDisabled(), `${app.name}: back to 跟随串流 puts 保存 off: a fixed display that was never filled in leaves nothing behind`)
      assert.equal(host.writes().length, 0, `${app.name}: nothing was written`)
      await page.press(page.radios(ZH.displayFixed)[0])
      await page.press(page.button('保存'))
      const puts = host.writes().filter((c) => c.method === 'PUT')
      assert.equal(puts.length, 1)
      const display = puts[0].body.display
      assert.equal(display.mode, 'fixed')
      for (const value of Object.values(display)) assert.equal(typeof value, 'string', `${app.name}: every value of the display is text (the generic apps writer keeps nothing else)`)
      page.unmount()
    })
  })
}
