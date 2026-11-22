import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

// 帮助与关于 of the Web UI: the four tiles first, then the way to 关于; the About page keeps its own layout (no tiles)
const read = (p) => fs.readFileSync(new URL(p, import.meta.url), 'utf8')

test('the help rail opens 帮助与关于 (the tiles and the way to 关于); 关于 opens over it', () => {
  const shell = read('../views/WebShell.vue')
  const help = shell.indexOf("'help-page': { rail: 'help', list: false")
  const about = shell.indexOf("  about: { rail: 'help', rootRail: 'about', list: false, push: true")
  assert.ok(help > 0 && about > help, 'the help page is the first page of the help rail, so the rail opens it')
  assert.match(shell, /'help-page': lazy\(\(\) => import\('\.\/Help\.vue'\)\)/)
  assert.match(shell, /'help-page': 'help' \}/)
})

test('the tiles are on 帮助与关于 and nowhere else: not on the About page', () => {
  const help = read('../views/Help.vue')
  assert.match(help, /<MHelpTiles/)
  assert.match(help, /@open-update="shell\?\.nav\.go\('about', \{ openUpdate: true \}\)"/, 'an update waits on 关于, where the update sheet is: pressing 更新 opens it there already open')
  assert.match(read('../views/About.vue'), /const sheet = ref\(!!props\.params\?\.openUpdate\)/, 'the About page opens the sheet only when asked: the ordinary way in leaves it closed')
  assert.match(help, /@doctor="shell\?\.nav\.go\('doctor', \{ autostart: true \}\)"/)
  assert.match(help, /@downloads="shell\?\.nav\.go\('downloads'\)"/)
  assert.match(help, /aboutLine\(installed\.value, \{ state: update\.state\.value \}/, 'the second line of 关于 says the update state')
  const about = read('../views/About.vue')
  assert.ok(!/MHelpTiles/.test(about), 'the About page has no tiles')
  assert.match(about, /<template #help>/, 'its own help rows are as they were')
})
