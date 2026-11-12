import test from 'node:test'
import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { readdirSync, readFileSync, statSync } from 'node:fs'
import { dirname, join, relative, sep } from 'node:path'
import { fileURLToPath } from 'node:url'

// The one mascot of the product is the yellow lion girl. No other character's name may be in shipped source (the name Gura and the
// like), and no image of the other characters may ship. The word is matched as a token: "configuration" contains the letters but is not it.
const here = dirname(fileURLToPath(import.meta.url))
const ROOT = join(here, '../../../../..')
const DIRS = ['src_assets', 'src', 'cmake', 'assets', 'gamebar-widget', 'README.md', 'CMakeLists.txt']
const SKIP = new Set(['node_modules', 'dist', 'target', 'build', '.git', 'third-party', 'ssl'])
const NAME = /(?<![a-z])gura(?![a-z])|(?<![a-z])gura(?=[A-Z_])|gawr|shark|鲨|基地娘|(?<![a-z])sunshine[ -]?girl(?![a-z])/i
// sha256 of images that were shipped and show another character (the reviewed list); they must not come back
const BLOCKED_IMAGES = new Map([
  ['53b38badafb38f5b533fd337a373d74b12a6c55866564ee4dd0e09731aabcf4a', 'the old application icon'],
  ['cc7910cddea18d49001c7cf50f718546d6a787a2b098fa2208542cb9fafe1750', 'the old desktop-alt cover (shark fins)'],
  ['640c06265fb5d3436dadbfdbb53a14f97f09f3b3d230d1efaa62011991c13883', 'sunshine-bg0.webp, the old default wallpaper'],
])
const IMAGE = /\.(png|jpe?g|webp|gif|bmp|ico|icns|svg)$/i

function* walk(dir) {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    if (SKIP.has(entry.name)) continue
    const full = join(dir, entry.name)
    if (entry.isDirectory()) yield* walk(full)
    else yield full
  }
}
const files = () => DIRS.flatMap((d) => { const p = join(ROOT, d); try { return statSync(p).isDirectory() ? [...walk(p)] : [p] } catch { return [] } })
const self = fileURLToPath(import.meta.url)

test('no other character is named in shipped source', () => {
  const hits = []
  for (const file of files()) {
    if (file === self || IMAGE.test(file) || /\.(woff2?|ttf|exe|dll|node|zip|bin)$/i.test(file) || statSync(file).size > 3 * 1024 * 1024) continue
    const text = readFileSync(file, 'utf8')
    if (text.includes('\0')) continue
    text.split('\n').forEach((line, i) => { if (NAME.test(line)) hits.push(`${relative(ROOT, file).split(sep).join('/')}:${i + 1}: ${line.trim().slice(0, 100)}`) })
  }
  assert.deepEqual(hits, [])
})

test('no shipped image is one of the known pictures of another character, and the old default wallpaper is not used', () => {
  const found = []
  for (const file of files()) {
    if (!IMAGE.test(file)) continue
    const known = BLOCKED_IMAGES.get(createHash('sha256').update(readFileSync(file)).digest('hex'))
    if (known) found.push(`${relative(ROOT, file)}: ${known}`)
  }
  assert.deepEqual(found, [])
  const urls = []
  for (const file of files()) {
    if (file === self || IMAGE.test(file) || statSync(file).size > 3 * 1024 * 1024) continue
    if (/sunshine-bg0|t\.alcy\.cc|109527119_p1/.test(readFileSync(file, 'utf8'))) urls.push(relative(ROOT, file))
  }
  assert.deepEqual(urls, [])
})
