import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync, readdirSync } from 'node:fs'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { ICONS } from 'foundation-ui/icons'

// The glyphs the shipped design system carries. Material Symbols (Rounded, weight 400) is the current
// Google symbol set the design system builds its glyphs from; every icon name the Web UI draws must be
// one of these, or the icon component renders nothing (source index: ds5-material-symbols-round-codepoints-20261105).
const SHIPPED = new Set(Object.keys(ICONS))
const WEB_ROOT = fileURLToPath(new URL('..', import.meta.url)).replace(/\/$/, '')

// Words quoted inside icon maps and icon expressions that are not icon names: the device kinds of the
// reverse map in utils/clientList.js and the values compared in template conditions.
const NOT_ICON_WORDS = new Set(['phone', 'tablet', 'computer', 'tv', 'steam', 'headset', 'reachable', 'sparxie'])

const skipDir = /(^|\/)(node_modules|tests?|fixtures|dist|build|public)(\/|$)/
const sources = []
const walk = (dir) => {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const path = join(dir, entry.name)
    if (entry.isDirectory()) { if (!skipDir.test(path)) walk(path) } else if (/\.(js|vue)$/.test(entry.name)) sources.push(path)
  }
}
walk(WEB_ROOT)

// The span of an object or array literal, so nested braces inside an icon map do not end it early.
const literalBody = (source, openIndex) => {
  const open = source[openIndex]
  const close = open === '{' ? '}' : ']'
  let depth = 0
  for (let i = openIndex; i < source.length; i++) {
    if (source[i] === open) depth++
    else if (source[i] === close && --depth === 0) return source.slice(openIndex, i + 1)
  }
  return ''
}

// The expressions that set an icon: 'icon: <expr>' in a script and ':icon="<expr>"' in a template.
const iconExpressions = function* (source) {
  for (const match of source.matchAll(/:(?:icon|name|title-icon|trail-icon|lead-icon|icon-name)="([^"]+)"/g)) yield match[1]
  for (const match of source.matchAll(/(?<![:\w-])icon\s*:\s*/g)) {
    let depth = 0
    let i = match.index + match[0].length
    const start = i
    for (; i < source.length; i++) {
      const c = source[i]
      if (c === '(' || c === '[' || c === '{') depth++
      else if (c === ')' || c === ']' || c === '}') { if (depth === 0) break; depth-- }
      else if ((c === ',' && depth === 0) || c === '\n') break
    }
    yield source.slice(start, i)
  }
}

const drawn = new Map()
const record = (name, file, what) => { if (name && !NOT_ICON_WORDS.has(name) && !drawn.has(name)) drawn.set(name, `${file} (${what})`) }
const quoted = (text) => [...text.matchAll(/['"]([a-z][a-z0-9_]*)['"]/g)].map((match) => match[1])

for (const file of sources) {
  const source = readFileSync(file, 'utf8')
  const relative = file.slice(WEB_ROOT.length + 1)
  // a name written as the icon of something: icon: 'name', icon="name", <MIcon name="name">
  for (const match of source.matchAll(/(?<![:\w-])(?:icon|iconName|arrowIcon|titleIcon|trailIcon|leadIcon|statusIcon)\s*[:=]\s*['"]([a-z][a-z0-9_]*)['"]/g)) record(match[1], relative, 'icon property')
  for (const match of source.matchAll(/(?<![:\w-])(?:icon|title-icon|trail-icon|lead-icon|icon-name)=['"]([a-z][a-z0-9_]*)['"]/g)) record(match[1], relative, 'icon attribute')
  for (const match of source.matchAll(/<MIcon\b[^>]*?(?<![:\w-])name=['"]([a-z][a-z0-9_]*)['"]/g)) record(match[1], relative, 'icon component')
  for (const expr of iconExpressions(source)) {
    // both branches of a choice are drawn
    for (const match of expr.matchAll(/\?\s*['"]([a-z][a-z0-9_]*)['"]\s*:\s*['"]([a-z][a-z0-9_]*)['"]/g)) { record(match[1], relative, 'icon choice'); record(match[2], relative, 'icon choice') }
    // a quoted word in an icon expression is a name unless it is a compared value or a device kind
    for (const name of quoted(expr)) record(name, relative, 'icon expression')
  }
  // a map or list of icon names, a bare name bound to an ICON-named constant, and an icons property
  for (const match of source.matchAll(/\b(?:const|let|var)\s+[A-Za-z_]*[Ii][Cc][Oo][Nn][A-Za-z_]*\s*=\s*['"]([a-z][a-z0-9_]*)['"]/g)) record(match[1], relative, 'icon constant')
  for (const match of source.matchAll(/\b(?:const|let|var)\s+[A-Za-z_]*[Ii][Cc][Oo][Nn][A-Za-z_]*\s*=\s*[[{]/g)) {
    const body = literalBody(source, match.index + match[0].length - 1)
    for (const name of quoted(body)) record(name, relative, 'icon map')
  }
  for (const match of source.matchAll(/\bicons\s*:\s*(\[[^\]]*\]|\{[^}]*\})/g)) for (const name of quoted(match[1])) record(name, relative, 'icons property')
}

test('the scan finds the Web sources and their icon names', () => {
  assert.ok(SHIPPED.size > 200, 'the shipped icon set of the design system loaded')
  assert.ok(sources.length > 150, 'the Web sources were found')
  assert.ok(drawn.size > 60, 'icon usages were found')
})

test('every icon name the Web UI draws is in the shipped icon set', () => {
  const missing = [...drawn].filter(([name]) => !SHIPPED.has(name))
  assert.equal(missing.length, 0, 'icons the shipped set does not carry:\n' + missing.map(([name, where]) => `${name} at ${where}`).join('\n'))
})
