// vite-build-20261117, Building for Production. Measure shipped bytes, not source maps.
import fs from 'node:fs/promises'
import path from 'node:path'
import { deflateRawSync } from 'node:zlib'
import { realpathSync } from 'node:fs'
import { performance } from 'node:perf_hooks'
import { build } from 'vite'

const [source, destination] = process.argv.slice(2)
if (!source || !destination) throw new Error('usage: measure-web-package.mjs SOURCE OUTPUT')
const root = path.resolve(source)
const output = path.resolve(destination)
// Keep the two trees disjoint: publicDir is copied recursively by Vite, and emptyOutDir would
// wipe a SOURCE that lives inside OUTPUT. Compare real paths (symlinks resolved, case folded where
// the file system ignores case) by their relative path, so a root OUTPUT and a link are caught too.
function real(location) {
  let head = path.resolve(location)
  const tail = []
  for (;;) {
    try { head = realpathSync.native(head); break } catch {
      const parent = path.dirname(head)
      if (parent === head) break
      tail.unshift(path.basename(head)); head = parent
    }
  }
  return path.join(head, ...tail)
}
const fold = process.platform === 'win32' || process.platform === 'darwin' ? (location) => location.toLowerCase() : (location) => location
const inside = (child, parent) => {
  const rel = path.relative(fold(parent), fold(child))
  return rel === '' || (rel !== '..' && !rel.startsWith('..' + path.sep) && !path.isAbsolute(rel))
}
if (inside(real(output), real(root)) || inside(real(root), real(output))) throw new Error('OUTPUT and SOURCE must not be the same or contain each other')
// An exported value would move Vite's root away from SOURCE (vite.config.js) and out of the guard above.
delete process.env.SUNSHINE_SOURCE_ASSETS_DIR
process.chdir(root)
const chunks = []
const started = performance.now()
await build({
  configFile: path.join(root, 'scripts/vite/vite.config.js'),
  build: { outDir: output },
  plugins: [{
    name: 'package-module-sizes',
    generateBundle(_options, bundle) {
      for (const chunk of Object.values(bundle)) {
        if (chunk.type !== 'chunk') continue
        chunks.push({
          name: chunk.fileName, imports: chunk.imports, dynamicImports: chunk.dynamicImports,
          modules: Object.entries(chunk.modules).map(([id, details]) => ({
            // Strip checkout locations from the shareable summary.
            name: id.includes('/node_modules/') ? 'npm/' + id.split('/node_modules/').at(-1)
              : id.includes('/src_assets/') ? 'web/' + id.split('/src_assets/').at(-1)
                : id.includes('/src/') ? 'shared/' + id.split('/src/').at(-1) : path.basename(id),
            rendered_bytes: details.renderedLength,
          })).sort((a, b) => b.rendered_bytes - a.rendered_bytes),
        })
      }
    },
  }],
})
const built = performance.now()
async function filesIn(directory) {
  const rows = []
  for (const entry of await fs.readdir(directory, { withFileTypes: true })) {
    const file = path.join(directory, entry.name)
    if (entry.isDirectory()) rows.push(...await filesIn(file))
    else {
      const bytes = await fs.readFile(file)
      rows.push({ name: path.relative(output, file).split(path.sep).join('/'), raw_bytes: bytes.length,
        deflate_bytes: deflateRawSync(bytes, { level: 9 }).length })
    }
  }
  return rows
}
const files = await filesIn(output)
const result = {
  method: 'Production Vite build; per-file raw DEFLATE level 9, excluding ZIP container overhead; not Inno Setup size',
  build_ms: built - started, compress_and_read_ms: performance.now() - built,
  raw_bytes: files.reduce((n, file) => n + file.raw_bytes, 0),
  deflate_bytes: files.reduce((n, file) => n + file.deflate_bytes, 0),
  files: files.sort((a, b) => b.raw_bytes - a.raw_bytes), chunks,
}
await fs.writeFile(output + '.json', JSON.stringify(result, null, 2) + '\n')
console.log(JSON.stringify({ raw_bytes: result.raw_bytes, deflate_bytes: result.deflate_bytes,
  build_ms: result.build_ms, compress_and_read_ms: result.compress_and_read_ms }))
