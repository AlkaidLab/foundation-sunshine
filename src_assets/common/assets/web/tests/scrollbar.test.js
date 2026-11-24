import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

// The Web UI has no scrollbar rule of its own: foundation-ui's css/scrollbar.css draws every scrollbar (thin, wider under the pointer, pill, Material colour roles). A rule here with
// scrollbar-width: thin / auto or scrollbar-color switches that off for its element, and a ::-webkit-scrollbar rule draws a second scrollbar. `scrollbar-width: none` (a strip that hides its bar) stays.
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const walk = (dir) => fs.readdirSync(dir, { withFileTypes: true }).flatMap((e) => (e.isDirectory() ? (e.name === 'node_modules' || e.name === 'tests' ? [] : walk(path.join(dir, e.name))) : [path.join(dir, e.name)]))
test('no stylesheet or component of the Web UI draws a scrollbar of its own', () => {
  const bad = []
  for (const f of walk(root).filter((x) => /\.(less|css|vue)$/.test(x))) {
    const text = fs.readFileSync(f, 'utf8')
    for (const m of text.matchAll(/::-webkit-scrollbar(?![\w-]*\s*\{\s*display:\s*none)|scrollbar-width:\s*(thin|auto)|scrollbar-color:/g)) bad.push(`${path.relative(root, f)}: ${m[0]}`)
  }
  assert.deepEqual(bad, [])
})
