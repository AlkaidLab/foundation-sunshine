import assert from 'node:assert/strict'
import { access, mkdir, mkdtemp, readdir, readFile, rm, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import test from 'node:test'
import { brotliDecompressSync } from 'node:zlib'
import { assetEncoding, compressAssets } from '../../../../../scripts/vite/compressAssets.js'

// Exercise the actual build plugin: packages ship each text asset exactly
// once, as the Brotli copy the host negotiates or decodes. The plain file and
// the digest markers of the earlier sidecar layout must not come back.
test('build compression replaces text assets with a single brotli copy', async (t) => {
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

  assert.equal(assetEncoding, 'br')
  const compressed = await readFile(stable + '.br')
  assert.equal(brotliDecompressSync(compressed).toString(), 'abc')
  assert.equal(brotliDecompressSync(await readFile(hashed + '.br')).toString(), 'abc')
  await assert.rejects(access(stable), { code: 'ENOENT' })
  await assert.rejects(access(hashed), { code: 'ENOENT' })
  await assert.rejects(access(stable + '.gz'), { code: 'ENOENT' })
  await assert.rejects(access(stable + '.gz.sha256'), { code: 'ENOENT' })
  await assert.rejects(access(stable + '.br.sha256'), { code: 'ENOENT' })
  const names = (await readdir(assets, { recursive: true })).sort()
  assert.deepEqual(names, [
    'generated',
    join('generated', 'sample-abcdefgh.js.br'),
    'locale',
    join('locale', 'sample.json.br'),
  ])
})

