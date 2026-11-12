import test from 'node:test'
import assert from 'node:assert/strict'
import { cpSync, existsSync, mkdtempSync, readdirSync, readFileSync, rmSync, statSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { foundationUiAliases, resolveFoundationUi } from '../../../../../scripts/vite/foundationUi.js'

const webRoot = join(dirname(fileURLToPath(import.meta.url)), '..')
const m3e = resolve(fileURLToPath(import.meta.resolve('foundation-ui/vue')), '../../..')

const walk = (dir) => readdirSync(dir).flatMap((name) => {
  const full = join(dir, name)
  if (name === 'node_modules') return []
  return statSync(full).isDirectory() ? walk(full) : /\.(js|vue|mjs)$/.test(name) ? [full] : []
})
const specifiers = [...new Set(walk(webRoot).flatMap((file) => [...readFileSync(file, 'utf8').matchAll(/['"`](foundation-ui(?:\/[^'"`\s]*)?)['"`]/g)].map((m) => m[1])))]
  .filter((s) => !/[${}*]/.test(s) && !s.endsWith('/') && s !== 'foundation-ui')

test('every foundation-ui import of the Web UI is found by the build aliases, inside the checkout the build was told about', () => {
  assert.ok(specifiers.length > 5, 'the scan found the imports')
  for (const specifier of specifiers) {
    const target = resolveFoundationUi(m3e, specifier)
    assert.ok(target && target.startsWith(`${m3e}/`), `${specifier} -> ${target}`)
    assert.ok(existsSync(target), `${specifier} -> ${target} does not exist`)
  }
})

test('with the checkout elsewhere than node_modules/foundation-ui, nothing is resolved through node_modules: one copy of the unsaved-changes store', () => {
  const copy = mkdtempSync(join(tmpdir(), 'm3e-copy-'))
  try {
    cpSync(join(m3e, 'package.json'), join(copy, 'package.json'))
    for (const specifier of ['foundation-ui/pending', 'foundation-ui/vue', 'foundation-ui/notify', 'foundation-ui/dialog', 'foundation-ui/auth', 'foundation-ui/settings/index.js', 'foundation-ui/vue/guide.js']) {
      const target = resolveFoundationUi(copy, specifier)
      assert.ok(target?.startsWith(`${copy}/`), `${specifier} -> ${target}`)
    }
    // the store the settings report into (pending) and the one the bar reads (vue) are the same file
    assert.equal(dirname(resolveFoundationUi(copy, 'foundation-ui/pending')), join(copy, 'src/pending'))
    assert.ok(foundationUiAliases(copy).length >= Object.keys(JSON.parse(readFileSync(join(copy, 'package.json'), 'utf8')).exports).length)
  } finally { rmSync(copy, { recursive: true, force: true }) }
})

test('the build config takes its foundation-ui aliases from the package, not from a hand-kept list that leaves imports to node_modules', () => {
  const config = readFileSync(join(webRoot, '../../../../scripts/vite/vite.config.js'), 'utf8')
  assert.match(config, /\.\.\.foundationUiAliases\(foundationUi\)/)
  assert.doesNotMatch(config, /find: \/\^foundation-ui/)
})
