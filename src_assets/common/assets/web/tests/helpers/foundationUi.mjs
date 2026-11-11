// Lets the Node test runner resolve the pure modules of the shared package the way the build does (a Vite alias).
// Import this first and load the code under test with dynamic imports afterwards.
import { register } from 'node:module'
import { pathToFileURL } from 'node:url'
import { resolve } from 'node:path'

const root = process.env.FOUNDATION_UI_DIR || resolve(import.meta.dirname, '../../../../../../../ds5-ui-m3e')
const hook = `
const root = ${JSON.stringify(pathToFileURL(root + '/').href)}
export async function resolve(specifier, context, next) {
  const c = /^foundation-ui\\/clients$/.test(specifier)
  if (c) return { url: new URL('src/clients/catalogue.js', root).href, shortCircuit: true }
  const m = /^foundation-ui\\/(vue\\/guide\\.js|settings\\/.+\\.js|nav\\/.+\\.js|about\\/.+\\.js|vue\\/deviceTiles\\.js|diagnostics\\/.+\\.js)$/.exec(specifier)
  if (m) return { url: new URL('src/' + m[1], root).href, shortCircuit: true }
  return next(specifier, context)
}
`
register('data:text/javascript,' + encodeURIComponent(hook))
