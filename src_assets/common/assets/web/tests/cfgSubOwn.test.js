import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

// A stale section left by another tab hid every row of 常规 (an empty page with only a Back button, a reported regression): the tabs drop a section that is not theirs.
const read = (p) => fs.readFileSync(new URL(`../${p}`, import.meta.url), 'utf8')
const idsOf = (file) => [...read(file).matchAll(/<CfgSub\s+id="([^"]+)"([^>]*)>/g)].map((m) => ({ id: m[1], inline: /\binline\b/.test(m[2]) }))

test('Config.vue drops a section that is not one of the tab\'s own, and the tab\'s own are exactly the CfgSub ids of its page', () => {
  const view = read('views/Config.vue')
  const own = JSON.parse(view.match(/const OWN_SUBS = (\{[^\n]*\})/)[1].replace(/(\w+):/g, '"$1":').replace(/'/g, '"'))
  for (const [tab, file] of [['general', 'configs/tabs/General.vue'], ['network', 'configs/tabs/Network.vue'], ['files', 'configs/tabs/Files.vue']]) {
    const subs = idsOf(file)
    assert.deepEqual(subs.map((s) => s.id).sort(), [...own[tab]].sort(), `${tab}: the sections of the page are the sections the guard keeps`)
  }
  assert.match(view, /if \(current && OWN_SUBS\[id\] && !OWN_SUBS\[id\]\.includes\(current\)\) nextTick\(\(\) => \{ if \(subNav\.current\.value === current\) subNav\.open\('', true\) \}\)/)
  assert.match(read('components/cfg/CfgSub.vue'), /inline \? !nav\.sub\.value : nav\.sub\.value === id/, 'the rows of an inline section show only while no section is open: why a stale one blanks the page')
})
