import fs from 'node:fs'
import { resolve } from 'node:path'

// Every `foundation-ui/...` import of the Web UI is resolved INSIDE the one checkout the build was told about (FOUNDATION_UI_DIR, else the sibling ds5-ui-m3e),
// from the package's own `exports` map. Imports that were not aliased went through node_modules/foundation-ui: when that is another directory than the
// checkout (a copy, a worktree, a link to another tree) the bundle held the module twice, and a module that keeps state (the unsaved-changes store) was two
// stores: the settings reported into one, the bar read the other, and the Web UI showed no unsaved bar at all.
const escape = (text) => text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')

export function foundationUiAliases(dir) {
  const pkg = JSON.parse(fs.readFileSync(resolve(dir, 'package.json'), 'utf8'))
  const out = []
  for (const [key, target] of Object.entries(pkg.exports || {})) {
    if (typeof target !== 'string') continue
    if (key === '.') { out.push({ find: /^foundation-ui$/, replacement: resolve(dir, target) }); continue }
    const name = `foundation-ui/${key.slice(2)}`
    if (name.includes('*')) {
      const [before, after] = name.split('*')
      out.push({ find: new RegExp(`^${escape(before)}(.+)${escape(after)}$`), replacement: resolve(dir, target.replace('*', '$1')) })
    } else {
      out.push({ find: new RegExp(`^${escape(name)}$`), replacement: resolve(dir, target) })
    }
  }
  // a file the package does not list (foundation-ui/vue/guide.js): the same tree
  out.push({ find: /^foundation-ui\/(.+)$/, replacement: resolve(dir, 'src/$1') })
  return out
}

/** Where an import of the Web UI lands, by the same rules as the aliases (the first that matches). */
export function resolveFoundationUi(dir, specifier) {
  for (const { find, replacement } of foundationUiAliases(dir)) if (find.test(specifier)) return specifier.replace(find, replacement)
  return null
}
