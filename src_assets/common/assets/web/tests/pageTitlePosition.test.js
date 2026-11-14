import test from 'node:test'
import assert from 'node:assert/strict'
import { readFile, readdir } from 'node:fs/promises'
import { join } from 'node:path'

// Every page is one column (foundation-ui layout.css): the title and the content share its left edge, the pane's inset under the column's maximum and centred above it. The Web UI sets the width of a page's own column (--m-column) and nothing about its title row, and no page centres itself;
// a page in the panel's frame also has no margin of the document around it (it moved every framed title 8 px right and down).
const web = new URL('../', import.meta.url).pathname
const files = []
const walk = async (dir) => {
  for (const e of await readdir(dir, { withFileTypes: true })) {
    const p = join(dir, e.name)
    if (e.isDirectory()) { if (e.name !== 'tests' && e.name !== 'node_modules') await walk(p) }
    else if (/\.(vue|css|less)$/.test(e.name)) files.push(p)
  }
}
await walk(web)

test('no stylesheet or style block of the Web UI styles the page title row', async () => {
  assert.ok(files.length > 20)
  const hits = []
  for (const f of files) {
    const text = await readFile(f, 'utf8')
    for (const m of text.matchAll(/[^{}\n]*\.m-page(__title|__bar\b|--(sub|wide|full|cols)\b[^{}]*\.m-page__(title|bar))[^{}]*\{([^{}]*)\}/g)) {
      if (/^\s*background\s*:\s*transparent\s*;?\s*$/.test(m[4])) continue // the wallpaper shows through the bar: no position
      hits.push(`${f.slice(web.length)}: ${m[0].trim().slice(0, 100)}`)
    }
  }
  assert.deepEqual(hits, [])
})

test('the document of a page in the panel frame has no margin (foundation-ui layout.css)', async () => {
  const layout = await readFile(new URL('../../../../../node_modules/foundation-ui/src/css/layout.css', import.meta.url), 'utf8')
  assert.match(layout, /html:has\(\.m-web-embedded\) body \{ margin: 0; \}/)
})

test('title left equals content left, centred once the pane is wider than the column: no column of the Web UI is pinned left or centred by a band of its own', async () => {
  const less = await readFile(new URL('../styles/m3e-web.less', import.meta.url), 'utf8')
  const column = less.match(/\.home-page > \.container,[^{]*\{([^{}]*)\}/)[1]
  assert.match(column, /width: min\(100%, 1024px\); max-width: 1024px; margin-inline: auto; padding-inline: 32px;/, 'the legacy pages are the 960 px column plus the inset on each side')
  for (const f of files) {
    const text = await readFile(f, 'utf8')
    assert.doesNotMatch(text, /--m-title-(band|off|gutter)/, `${f.slice(web.length)} uses a variable of the removed rule`)
  }
  const config = await readFile(new URL('../views/Config.vue', import.meta.url), 'utf8')
  assert.doesNotMatch(config, /--m-column/, 'a page keeps the shared width unless it is wide')
})
