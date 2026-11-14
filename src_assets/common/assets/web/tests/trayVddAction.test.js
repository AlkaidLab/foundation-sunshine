import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'

import { writeTrayVddKey } from '../composables/trayVddAction.js'

const asBrowser = async (t, reply) => {
  const originalFetch = globalThis.fetch
  globalThis.fetch = reply
  t.after(() => { globalThis.fetch = originalFetch })
}

test('the tray-owned keys are written with the tray action of their own name', async (t) => {
  const posted = []
  await asBrowser(t, async (url, options) => {
    posted.push({ url, options })
    return new Response(JSON.stringify({ status: true, message: 'VDD keep-enabled mode enabled' }), { status: 200, headers: { 'Content-Type': 'application/json' } })
  })

  await writeTrayVddKey('vdd_keep_enabled', true)
  await writeTrayVddKey('vdd_headless_create', false)

  assert.deepEqual(posted.map((p) => [p.url, JSON.parse(p.options.body)]), [
    ['/api/tray/action', { action: 'vdd_toggle_keep_enabled', enabled: true }],
    ['/api/tray/action', { action: 'vdd_toggle_headless_create', enabled: false }],
  ])
  for (const p of posted) {
    assert.equal(p.options.method, 'POST')
    assert.equal(new Headers(p.options.headers).get('Content-Type'), 'application/json')
  }
})

test('a refusal of the host becomes the error the control shows, such as the missing driver', async (t) => {
  await asBrowser(t, async () => new Response(JSON.stringify({ status: false, error: 'VDD_DRIVER_NOT_INSTALLED: install ZakoVDD from the Sunshine desktop VDD settings first' }), { status: 200, headers: { 'Content-Type': 'application/json' } }))
  await assert.rejects(writeTrayVddKey('vdd_keep_enabled', true), /VDD_DRIVER_NOT_INSTALLED: install ZakoVDD/)
})

test('a browser the route does not authorize sees the HTTP failure, not a silent success', async (t) => {
  await asBrowser(t, async () => new Response('', { status: 403 }))
  await assert.rejects(writeTrayVddKey('vdd_keep_enabled', true), /HTTP 403/)
})

test('a key that is not tray-owned is refused here and never reaches the host', async (t) => {
  await asBrowser(t, async () => { throw new Error('fetch must not be called') })
  await assert.rejects(writeTrayVddKey('output_name', true), /not a tray-owned virtual-display key: output_name/)
})

test('the settings store writes the tray-owned keys through the tray action when it runs in a plain browser', () => {
  // the store itself needs the component runtime; what is checked here is the wiring of its adapter
  const source = readFileSync(new URL('../composables/useVddSettings.js', import.meta.url), 'utf8')
  assert.match(source, /adapter\.setProtected = \(key, enabled\) => writeTrayVddKey\(key, enabled\)/)
  assert.match(source, /if \(invoke\) \{[\s\S]*?set_vdd_keep_enabled[\s\S]*?\}[\s\n]*else \{[\s\S]*?writeTrayVddKey/)
})
