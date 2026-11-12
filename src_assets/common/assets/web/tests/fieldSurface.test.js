import test from 'node:test'
import assert from 'node:assert/strict'
import { readFile, readdir } from 'node:fs/promises'
import { join } from 'node:path'

// The rule of a text field lives in foundation-ui (fields.css). The Web UI's fields are MField / MFieldItem (they carry the container), and its native inputs are .m-input:
// no page paints a field itself.
const root = new URL('..', import.meta.url).pathname
const walk = async (dir) => (await Promise.all((await readdir(dir, { withFileTypes: true })).map(async (e) => (e.name === 'node_modules' || e.name === 'tests' ? [] : e.isDirectory() ? walk(join(dir, e.name)) : e.name.endsWith('.vue') ? [join(dir, e.name)] : [])))).flat()
const files = await walk(root)

test('the Web UI draws no md-outlined-text-field of its own: fields are MField or MFieldItem, which carry the container fill', async () => {
  for (const f of files) assert.doesNotMatch(await readFile(f, 'utf8'), /<md-outlined-text-field\b/, f)
})

test('a native text input of a page is .m-input and its own rule sets size and type only', async () => {
  const table = await readFile(new URL('../components/CommandTable.vue', import.meta.url), 'utf8')
  assert.equal((table.match(/<input type="text" class="cmd-cell/g) || []).length, 4)
  assert.equal((table.match(/class="cmd-cell(?: cmd-cell--code)? m-input"/g) || []).length, 4)
  const rule = table.slice(table.indexOf('.cmd-cell {'), table.indexOf('&--code'))
  assert.doesNotMatch(rule.replace(/border-radius: \d+px;/, ''), /border|background/)
  const hex = await readFile(new URL('../components/dualsense/ColorWell.vue', import.meta.url), 'utf8')
  assert.match(hex, /<input class="ds5-hex m-input" type="text"/)
  assert.doesNotMatch(hex.match(/^\.ds5-hex \{[^\n]*\}/m)[0].replace('border-radius', ''), /border|background/)
})

test('no page of the Web UI paints a field outline of its own (a picture dragged over a cover is shown on the cover frame, not on the field)', async () => {
  for (const f of files) {
    const src = await readFile(f, 'utf8')
    assert.doesNotMatch(src, /--md-outlined-text-field-outline-color/, f)
  }
})
