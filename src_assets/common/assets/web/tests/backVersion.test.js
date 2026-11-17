import test from 'node:test'
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'

// the version in the caption bar, the update notice and the global back, as the Web UI shell wires them (the shell needs the bundler: its wiring is read from the source)
const read = (p) => readFile(new URL(p, import.meta.url), 'utf8')

test('the caption bar carries the back button, the version and the update line; the scene draws the chip', async () => {
  const shell = await read('../views/WebShell.vue')
  assert.match(shell, /<template v-if="backButton" #back><MCaptionBack :page="backPage"/)
  assert.match(shell, /<template #version><MCaptionVersion :version="shellVersion" :build="buildInfo"[^>]*:update="notice\.version"[^>]*@open-update="nav\.go\('about'\)"/)
  assert.match(shell, /:caption-height="48" :jumps="jumps" :chip="backChip"/)
  assert.match(shell, /<template #back-chip><MBackChip :page="backPage"/)
})
test('the Web UI mirrors the update notice from its own quiet check (useUpdateStatus); the dot is on 帮助与关于, where 关于 holds 软件更新', async () => {
  const shell = await read('../views/WebShell.vue')
  assert.match(shell, /watch\(\[\(\) => update\.state\.value, \(\) => update\.nextVersion\.value\]/)
  assert.match(shell, /r\.id === 'help' && notice\.value\.dot \? \{ badge: 'dot' \}/)
  assert.match(shell, /if \(page === 'about' && noticeState\.value\.available\) remember\(afterOpened\(noticeState\.value\)\)/)
  assert.doesNotMatch(shell, /r\.id === 'help' && update\.available\.value/)
})
test('the build identity is read once from /api/version, nothing per render', async () => {
  const shell = await read('../views/WebShell.vue')
  assert.equal((shell.match(/apiJson\('\/api\/version'\)/g) || []).length, 1)
  assert.match(shell, /onMounted\(async \(\) => \{ try \{ const v = await apiJson\('\/api\/version'\)/)
})
test('the pages move through webShell.nav and webShell.openConfig, which are jumps; the shell\'s own rail and list are not', async () => {
  const shell = await read('../views/WebShell.vue')
  assert.match(shell, /const jumpNav = \{ \.\.\.nav, go: \(id, params, opts\) => jumps\.jump\(\(\) => nav\.go\(id, params, opts\)\) \}/)
  assert.match(shell, /provide\('webShell', \{ hideList, nav: jumpNav, openConfig: jumpConfig,/)
  assert.match(shell, /const onRail = \(item\) => \(item\.id === 'settings-hub' \? nav\.go\('settings-hub'\) : nav\.selectRail\(item\.id\)\)/)
  assert.match(shell, /restore: \(place\) => \{ if \(place\.params\?\.tab \|\| place\.id === 'config'\) openConfig\(place\.params \|\| \{ tab: 'general' \}\); else nav\.go\(place\.id, place\.params\) \}/)
})
test('the search is its icon when the version leaves it no room; a phone has no title-bar version (its 帮助与关于 page, the 关于 page, shows it)', async () => {
  const shell = await read('../views/WebShell.vue')
  assert.match(shell, /:compact="compactSearch \|\| caption\.tight\.value"/)
  assert.match(shell, /const caption = useCaptionRoom\(\)/)
  assert.doesNotMatch(shell, /web-more-version/)
})
