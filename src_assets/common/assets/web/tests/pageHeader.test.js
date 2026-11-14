import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

const read = (p) => fs.readFileSync(new URL(p, import.meta.url), 'utf8')

test('no page of the Web UI opts into a cross-document view transition; the first frame is the colour of the theme, not a white canvas', () => {
  for (const file of ['../template_header.html', '../template_header_dev.html']) {
    const header = read(file)
    assert.ok(!/@view-transition/.test(header), `${file}: a cross-document transition holds the old page for about 1.5 s on WebView2 (measured, docs/telemetry of the RZ2 round: tools/r23-vt-wv2b.mjs)`)
    assert.match(header, /<meta name="color-scheme" content="light dark" \/>/, `${file}: an iframe document follows the embedder's scheme instead of an opaque white canvas`)
    assert.match(header, /foundation-ui\.boot/, `${file}: the surface colour of the last appearance paints the first frame`)
    assert.match(header, /root\.style\.backgroundColor = color/)
  }
})
