import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { undeclaredIdentifiers } from 'foundation-ui/testing/scope-check'

import { canOpenBigScreen, openBigScreenOnHost } from '../composables/openBigScreen.js'

const asBrowser = (t, reply) => {
  const originalFetch = globalThis.fetch
  globalThis.fetch = reply
  t.after(() => { globalThis.fetch = originalFetch })
}
const json = (body, status = 200) => new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } })
const read = (path) => readFileSync(new URL(path, import.meta.url), 'utf8')

test('the tile and the action exist on the host machine of a Windows host and nowhere else', () => {
  assert.equal(canOpenBigScreen({ platform: 'windows', local: true, hostname: '192.0.2.20' }), true, 'the host says the request comes from its own machine')
  assert.equal(canOpenBigScreen({ platform: 'windows', local: false, hostname: 'localhost' }), false, 'the host says it does not')
  assert.equal(canOpenBigScreen({ platform: 'windows', local: null, hostname: 'localhost' }), true, 'unknown: a loopback address of the page')
  assert.equal(canOpenBigScreen({ platform: 'windows', hostname: '127.0.0.1:47990' }), true)
  assert.equal(canOpenBigScreen({ platform: 'windows', hostname: '192.0.2.20' }), false, 'a browser on another machine')
  assert.equal(canOpenBigScreen({ platform: 'linux', local: true, hostname: 'localhost' }), false, 'the panel is a Windows component')
  assert.equal(canOpenBigScreen({ platform: 'macos', local: true }), false)
  assert.equal(canOpenBigScreen({}), false)
})

test('opening posts the open_desktop tray action and says why when the host refuses', async (t) => {
  const posted = []
  asBrowser(t, async (url, options) => { posted.push({ url, options }); return json({ status: true, message: 'Big screen requested' }) })
  await openBigScreenOnHost()
  assert.equal(posted[0].url, '/api/tray/action')
  assert.deepEqual(JSON.parse(posted[0].options.body), { action: 'open_desktop' })
  assert.equal(posted[0].options.method, 'POST')

  asBrowser(t, async () => json({ status: false, error: 'Foundation Desktop could not be started: file not found' }))
  await assert.rejects(openBigScreenOnHost(), /could not be started/)
})

test('the overview and the search of the Web UI use it, and the host has the tray action', () => {
  const home = read('../views/Home.vue')
  assert.match(home, /canOpenDesktop: canOpenBigScreen\(\{ platform: platform\.value, local: live\.local\.value, hostname: window\.location\.hostname \}\)/)
  assert.match(home, /case 'open-desktop': openBigScreenOnHost\(\)\.catch/)
  const shell = read('../views/WebShell.vue')
  assert.match(shell, /d\.id !== 'action:openDesktop' \|\| bigScreenHere\.value/)
  assert.match(shell, /tileDocs\(String\(uiLocale\.value\), \{ exclude: bigScreenHere\.value \? \[\] : \['act-open-desktop'\] \}\)/)
  for (const [name, source] of [['Home.vue', home], ['WebShell.vue', shell]]) assert.deepEqual(undeclaredIdentifiers(source, { filename: name }), [], name)
})
