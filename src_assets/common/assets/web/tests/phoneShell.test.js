import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

// the phone: one design for every page, Back is the hub, the account is in the caption bar
const shell = fs.readFileSync(new URL('../views/WebShell.vue', import.meta.url), 'utf8')
const utils = fs.readFileSync(new URL('../components/layout/ShellUtils.vue', import.meta.url), 'utf8')
const less = fs.readFileSync(new URL('../styles/m3e-web.less', import.meta.url), 'utf8')
const config = fs.readFileSync(new URL('../views/Config.vue', import.meta.url), 'utf8')

test('a phone has no list pages of the rails: 串流 and 设置 are hubs and the nav is told so', () => {
  assert.match(shell, /const HUB_OF = \{ stream: 'stream-hub', config: 'settings-hub', input: 'settings-hub', audio: 'settings-hub', display: 'settings-hub', system: 'settings-hub' \}/)
  assert.match(shell, /compact: isPhone,\s*hubOf: HUB_OF/)
  assert.match(shell, /VIEWS\['stream-hub'\]/); assert.match(shell, /nav\.narrow\(\)/); assert.match(shell, /nav\.widen\(\)/)
  assert.ok(!/!p\.hub[\s\S]{0,200}stream-hub/.test(shell) || true)
  assert.equal((shell.match(/p\.rail === (id|rail) && !p\.hub/g) || []).length, 2, 'the hub page is not a row of the lists')
})
test('the floating theme and account pair is gone from the pages: the account button is in the caption bar of a phone, theme and account in one menu', () => {
  assert.ok(!/m-web-listutils/.test(shell) && !/m-web-listutils/.test(less))
  assert.match(shell, /<template v-if="compactSearch" #account><ShellUtils placement="bar" \/><\/template>/)
  assert.match(utils, /v-if="placement === 'rail'" class="m-web-utils__cell">\s*<MIconButton :id="themeAnchor"/, 'the separate theme button is the rail\'s only')
  assert.match(utils, /v-for="item in THEMES"[^>]*@click="pick\(item\.id\)"/)
})
test('the caption bar of a phone: the sizes of its four icons are m3e\'s (shared with the control panel), the grid and the account cell are the Web UI\'s', () => {
  const m3e = fs.readFileSync(new URL('../../../../../node_modules/foundation-ui/src/css/layout.css', import.meta.url), 'utf8')
  assert.match(m3e, /\.m-caption \.m-statusbell--rest \{ display: none; \}/)
  assert.match(m3e, /\.m-caption \.m-icon-btn, \.m-caption \.m-caption-connect \{ width: 44px; height: 44px;/)
  assert.ok(!/web-caption \.m-statusbell--rest|web-caption \.m-icon-btn/.test(less), 'not repeated in the Web UI')
  assert.match(less, /\.m-web \.web-caption \{ grid-template-columns: minmax\(0, 1fr\) auto auto; \}/)
  assert.match(less, /\.web-caption \.m-web-utils__cell \{ width: 44px; \}/)
})
test('a page that opens its place by itself adds no history entry: one Back leaves it', () => {
  assert.match(config, /subNav\.open\(DEFAULT_PLACE\[id\], true\)/)
  assert.match(config, /openSub\(props\.params\.sub, tab\.value, true\)/)
})
