import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

// The control panel keeps one frame for the framed pages of the Web and tells it the page: the shell swaps the view, replaces (never pushes) the address, says that it can be told, and loads the other pages' chunks at idle.
const shell = fs.readFileSync(new URL('../views/WebShell.vue', import.meta.url), 'utf8')

test('an embedded shell swaps the view when the panel tells it the page, from the panel only, and says that it can be told', () => {
  assert.match(shell, /const embeddedId = ref\(idFromPath\(window\.location\.pathname\)\)/)
  assert.match(shell, /<component :is="VIEWS\[embeddedId\]" :key="embeddedId" \/>/)
  assert.match(shell, /event\.source !== window\.parent \|\| event\.data\?\.type !== 'foundation-frame-go'/)
  assert.match(shell, /const FRAMED_IDS = \['pin', 'apps', 'config', 'downloads', 'password'\]/)
  assert.match(shell, /history\.replaceState\(history\.state, '', path\)/)
  assert.doesNotMatch(shell, /history\.pushState/, 'Back is the panel\'s: the frame adds no history entry')
  assert.match(shell, /postMessage\(\{ type: 'foundation-frame-go-ready' \}, '\*'\)/)
})
test('the chunks of the framed pages are loaded at idle, so a swap finds them in memory', () => {
  assert.match(shell, /for \(const id of FRAMED_IDS\) VIEWS\[id\]\?\.__asyncLoader\?\.\(\)\.catch\(\(\) => \{\}\)/)
  assert.match(shell, /requestIdleCallback/)
})
