import { readdir, readFile, writeFile } from 'node:fs/promises'
import { extname, join, relative, sep } from 'node:path'
import { gzipSync } from 'node:zlib'
import { createHash } from 'node:crypto'

// Precompress static text at build time so the host can stream it without
// spending CPU on every request. Images and fonts are already compressed.
export function compressAssets() {
  return {
    name: 'compress-web-assets',
    apply: 'build',
    async writeBundle(options) {
      const assets = join(options.dir, 'assets')
      async function compressDirectory(dir) {
        for (const entry of await readdir(dir, { withFileTypes: true })) {
          const file = join(dir, entry.name)
          if (entry.isDirectory()) {
            await compressDirectory(file)
          } else if (entry.isFile() && ['.js', '.css', '.json'].includes(extname(entry.name))) {
            const original = await readFile(file)
            const compressed = gzipSync(original, { level: 9 })
            // Always emit a sidecar so an in-place upgrade overwrites the old
            // representation even when a file becomes very small.
            await writeFile(file + '.gz', compressed)
            // The host verifies both snapshots before using a sidecar for a
            // stable URL. In-place upgrades may otherwise mix two versions.
            if (relative(assets, file).split(sep)[0] !== 'generated') {
              const sha256 = (data) => createHash('sha256').update(data).digest('hex').toUpperCase()
              await writeFile(file + '.gz.sha256', sha256(original) + ' ' + sha256(compressed) + '\n')
            }
          }
        }
      }
      await compressDirectory(assets)
    },
  }
}
