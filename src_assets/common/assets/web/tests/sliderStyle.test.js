import { test } from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

// Every slider of the Web UI is the product slider: size m (the same 44 px row as s, a thicker track), a visible track and a value bubble from the shared component, icons at both ends where the setting has a less / more.
const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..')
const walk = (dir) => fs.readdirSync(dir, { withFileTypes: true }).flatMap((e) => (e.isDirectory() ? (e.name === 'tests' || e.name === 'node_modules' ? [] : walk(path.join(dir, e.name))) : [path.join(dir, e.name)]))
const files = walk(root).filter((f) => f.endsWith('.vue'))
const tags = (src) => [...src.matchAll(/<MSlider(?:Item)?\b[^>]*?(?:\/>|>)/gs)].map((m) => m[0])

test('no Web UI slider asks for the thin xs or s track, and none has a bare range input', () => {
  const bad = []
  for (const f of files) {
    const src = fs.readFileSync(f, 'utf8')
    for (const t of tags(src)) if (/\bsize="(xs|s)"/.test(t) || /:size="[^"]*'s'/.test(t)) bad.push(path.relative(root, f))
    if (/<input[^>]*type="range"/.test(src)) bad.push(`${path.relative(root, f)} (input range)`)
  }
  assert.deepEqual(bad, [])
})

test('the bitrate slider and the QVBR quality slider carry their icons', () => {
  const rate = fs.readFileSync(path.join(root, 'configs/tabs/audiovideo/BitrateControl.vue'), 'utf8')
  assert.match(rate, /icon-start="signal_cellular_alt_2_bar" icon-end="signal_cellular_alt"/)
  const amd = fs.readFileSync(path.join(root, 'configs/tabs/encoders/AmdAmfEncoder.vue'), 'utf8')
  assert.match(amd, /<MSliderItem[^>]*amd_qvbr_quality[^>]*size="m" icon-start="remove" icon-end="add"/)
})
