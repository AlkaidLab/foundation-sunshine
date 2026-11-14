import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

// The old interface (the pixel and hand-writing fonts, skewed text, the plum and cream 'morandi' palette) is gone from the Web UI and must not come back.
const web = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const OLD = [/PixelMplus/i, /YouYuan/i, /Fusion ?Pixel/i, /morandi/i, /skew\(/, /#2d2628/i, /#3d3235/i, /#d4a5a5/i, /#e6d5b8/i, /Indie Flower/i, /Kalam/, /Patrick Hand/i]
const EXT = /\.(vue|js|less|css|html|json)$/
function* walk(dir) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    if (['node_modules', 'tests', 'locale', 'i18n'].includes(entry.name)) continue
    const full = path.join(dir, entry.name)
    if (entry.isDirectory()) yield* walk(full)
    else if (EXT.test(entry.name)) yield full
  }
}

test('no file of the Web UI carries the old interface', () => {
  const hits = []
  for (const file of walk(web)) {
    const text = fs.readFileSync(file, 'utf8')
    for (const re of OLD) if (re.test(text)) hits.push(`${path.relative(web, file)}: ${re}`)
  }
  assert.deepEqual(hits, [])
})

test('the font files and the old token sheet are not shipped', () => {
  assert.equal(fs.existsSync(path.join(web, 'fonts')), false, 'fonts/ is back')
  assert.equal(fs.existsSync(path.join(web, 'styles/var.css')), false, 'styles/var.css is back')
  assert.doesNotMatch(fs.readFileSync(path.join(web, 'styles/welcome.less'), 'utf8'), /var\.css/)
})

test('the Web UI uses the shared component library instead of legacy icon and CSS runtimes', () => {
  const forbidden = [
    /@fortawesome/i,
    /font[- ]awesome/i,
    /(?:^|[^a-z])bootstrap(?:[^a-z]|$)/i,
    /\bfa-[a-z0-9-]+/,
  ]
  const hits = []
  for (const file of walk(web)) {
    if (file.endsWith('.test.js')) continue
    const text = fs.readFileSync(file, 'utf8')
    for (const re of forbidden) if (re.test(text)) hits.push(`${path.relative(web, file)}: ${re}`)
  }
  assert.deepEqual(hits, [])
})
