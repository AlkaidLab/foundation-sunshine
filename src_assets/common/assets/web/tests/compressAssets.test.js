import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import test from 'node:test'
import { gunzipSync } from 'node:zlib'
import { compressAssets } from '../../../../../scripts/vite/compressAssets.js'

// Exercise the actual build plugin: its gzip and marker are consumed by the
// C++ sidecar verifier. C++ also checks the same SHA-256 format in HTTP tests.
test('build compression emits matching gzip sidecars for fixed and hashed assets', async (t) => {
  const out = await mkdtemp(join(tmpdir(), 'sunshine-asset-compression-'))
  t.after(() => rm(out, { recursive: true, force: true }))
  const assets = join(out, 'assets')
  const stable = join(assets, 'locale', 'sample.json')
  const hashed = join(assets, 'generated', 'sample-abcdefgh.js')
  await mkdir(join(assets, 'locale'), { recursive: true })
  await mkdir(join(assets, 'generated'), { recursive: true })
  await writeFile(stable, 'abc')
  await writeFile(hashed, 'abc')
  await compressAssets().writeBundle({ dir: out })

  const compressed = await readFile(stable + '.gz')
  assert.equal(gunzipSync(compressed).toString(), 'abc')
  const digest = (data) => createHash('sha256').update(data).digest('hex').toUpperCase()
  assert.equal(await readFile(stable + '.gz.sha256', 'utf8'), `${digest('abc')} ${digest(compressed)}\n`)
  assert.equal(gunzipSync(await readFile(hashed + '.gz')).toString(), 'abc')
  await assert.rejects(readFile(hashed + '.gz.sha256'), { code: 'ENOENT' })
})
