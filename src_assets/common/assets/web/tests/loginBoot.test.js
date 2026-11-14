import assert from 'node:assert/strict'
import test from 'node:test'
import vm from 'node:vm'
import { readFileSync } from 'node:fs'

const dir = new URL('../', import.meta.url)
const script = readFileSync(new URL('login-boot.js', dir), 'utf8')
const defaults = JSON.parse(readFileSync(new URL('login-boot-defaults.json', dir), 'utf8'))
const html = readFileSync(new URL('login.html', dir), 'utf8')

// runs the inline script of the sign-in page in a small fake browser: what it puts in the head and on the root before the first paint
function run({ stored = {}, width = 1440, height = 900, dark = false } = {}) {
  const head = [], attrs = {}, classes = new Set()
  const store = { ...stored }
  const doc = { createElement: () => ({}), head: { appendChild: (e) => head.push(e) }, documentElement: { setAttribute: (k, v) => { attrs[k] = v }, classList: { add: (c) => classes.add(c) } } }
  const sandbox = { document: doc, localStorage: { getItem: (k) => (k in store ? store[k] : null) }, innerWidth: width, innerHeight: height, matchMedia: () => ({ matches: dark }), window: null }
  sandbox.window = sandbox
  vm.runInNewContext(script.replace('/*DEFAULTS*/null', () => JSON.stringify(defaults)), sandbox)
  return { head, attrs, classes }
}
const record = (over = {}) => JSON.stringify({ ...defaults.vivi2, ...over })

test('the page is built with the script in its head, before anything is painted', () => {
  assert.match(html, /<%- String\(header\)[^%]*%>\s*<style>html, body \{ margin: 0; \}<\/style>\s*<script><%- bootScript %><\/script>/)
  // the page runs under the notch and the home bar (the sheet keeps its safe-area insets) and has no 8 px body margin before its stylesheet arrives
  assert.match(html, /replace\('initial-scale=1\.0', 'initial-scale=1\.0, viewport-fit=cover'\)/)
  assert.ok(script.includes('/*DEFAULTS*/null'))
})

test('the bundled defaults are real records: the colours of both schemes, the frost and a tiny picture, for the wide and the other window shape', () => {
  for (const id of ['vivi1', 'vivi2']) {
    const r = defaults[id]
    assert.equal(r.v, 1); assert.match(r.css, /--md-sys-color-surface:/); assert.match(r.css, /\[data-bs-theme='dark'\]/)
    assert.match(r.thumb, /^data:image\/jpeg;base64,/); assert.ok(r.thumb.length < 20000)
    assert.ok(r.blur >= 24 && r.scrim.light > 0 && r.scrim.dark > 0)
    assert.equal(r.dark, 'tinted', '深色 is the default dark variant')
  }
})

test('a stored record is painted at once: the theme, the colours that beat the static tokens, the layer', () => {
  const { head, attrs, classes } = run({ stored: { 'foundation-ui.boot': record({ mode: 'dark' }) } })
  assert.equal(attrs['data-bs-theme'], 'dark')
  assert.ok(classes.has('m-boot'))
  const theme = head.find((e) => e.id === 'foundation-ui-boot-theme')
  assert.match(theme.textContent, /^html:root, \[data-bs-theme='light'\]/)
  assert.match(theme.textContent, /\nhtml\[data-bs-theme='dark'\]/)
  const layers = head.find((e) => e.id === 'foundation-ui-boot-layers')
  assert.match(layers.textContent, /html\.m-boot::after \{[^}]*url\("data:image\/jpeg/)
  assert.match(layers.textContent, /var\(--m-page-solid\) 51%/, 'the frost of the dark scheme')
  assert.match(layers.textContent, /var\(--m-fade-tone\) max\(51%, 94%\)/, 'paler towards the bottom, like the app')
})

test('the choice of the person decides light or dark: the record, the Web UI switch, else the system', () => {
  assert.equal(run({ stored: { 'foundation-ui.boot': record({ mode: 'light' }) }, dark: true }).attrs['data-bs-theme'], 'light')
  assert.equal(run({ stored: { 'foundation-ui.boot': record({ mode: 'system' }) }, dark: true }).attrs['data-bs-theme'], 'dark')
  assert.equal(run({ stored: { 'foundation-ui.boot': record({ mode: 'system' }), theme: 'light' }, dark: true }).attrs['data-bs-theme'], 'light')
})

test('with nothing stored, or a damaged record, the bundled default of the window shape is painted: no plain white page, no swap later', () => {
  const wide = run({ width: 1600, height: 900 }), tall = run({ width: 900, height: 1000 })
  for (const r of [wide, tall, run({ stored: { 'foundation-ui.boot': '{not json' } }), run({ stored: { 'foundation-ui.boot': JSON.stringify({ v: 9 }) } })]) {
    assert.ok(r.classes.has('m-boot')); assert.ok(r.head.some((e) => e.id === 'foundation-ui-boot-layers'))
  }
  const picture = (r) => /url\("([^"]+)"/.exec(r.head.find((e) => e.id === 'foundation-ui-boot-layers').textContent)[1]
  assert.equal(picture(wide), defaults.vivi2.thumb)
  assert.equal(picture(tall), defaults.vivi1.thumb)
})

test('a record that is not one of ours paints nothing and never throws', () => {
  const r = run({ stored: { 'foundation-ui.boot': JSON.stringify({ v: 1, css: 'body { display: none }' }) }, width: 1600, height: 900 })
  // the damaged record is replaced by the default of the shape, never used
  assert.ok(!r.head.some((e) => /display: none/.test(e.textContent || '')))
})
