#!/usr/bin/env node
// Adds the keys that a locale file lacks, taking their value from en.json, without touching any other line: no sorting, no
// re-indenting, no change to what is already there. A missing key goes in as the first entry of its parent object, so no line that is
// already there changes (not even by a comma); a whole missing object is written out in full. Untranslated values are English until someone translates them (the interface falls back to English
// anyway, this keeps `validate-i18n` satisfied).
//   node scripts/add-missing-locale-keys.mjs            add to every locale file
//   node scripts/add-missing-locale-keys.mjs --check    exit 1 when a file lacks a key (nothing is written)
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const LOCALES = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../src_assets/common/assets/web/public/assets/locale')
const isObject = (v) => v && typeof v === 'object' && !Array.isArray(v)

/** [{ parent: [keys], key, value }]: the subtrees of `base` that `tree` lacks, at the highest level they are missing. */
export function missingOf(base, tree, parent = []) {
  const out = []
  for (const [key, value] of Object.entries(base)) {
    if (!(key in tree)) out.push({ parent, key, value })
    else if (isObject(value) && isObject(tree[key])) out.push(...missingOf(value, tree[key], [...parent, key]))
  }
  return out
}

/** Offset of the `{` that opens the object at `keys` (a path of object keys from the root), found by scanning the text. */
function openOf(text, keys) {
  const opens = []
  const path = []
  let key = null
  for (let i = 0; i < text.length; i++) {
    const c = text[i]
    if (c === '"') {
      let j = i + 1
      while (text[j] !== '"') j += text[j] === '\\' ? 2 : 1
      let k = j + 1
      while (/\s/.test(text[k])) k++
      // a string followed by a colon is a key (of the object we are inside)
      if (text[k] === ':') key = JSON.parse(text.slice(i, j + 1))
      i = j
    } else if (c === '{') {
      if (opens.length > 0) path.push(key)
      opens.push(i)
      if (path.length === keys.length && path.every((p, n) => p === keys[n])) return i
      key = null
    } else if (c === '}') {
      opens.pop()
      if (opens.length > 0) path.pop()
    } else if (c === ',') key = null
  }
  throw new Error(`object ${keys.join('.')} not found`)
}

/**
 * The text with every missing key of `en` added. A new key is written as the first entry of its object, so no line that is
 * already there changes (not even by a comma).
 */
export function insertMissing(text, en) {
  let out = text
  const tree = JSON.parse(text)
  for (const { parent, key, value } of missingOf(en, tree).reverse()) {
    const open = openOf(out, parent)
    const lineStart = out.lastIndexOf('\n', open) + 1
    const indent = `${out.slice(lineStart, open).match(/^\s*/)[0]}  `
    const body = JSON.stringify(value, null, 2).split('\n').join(`\n${indent}`)
    let after = open + 1
    while (/\s/.test(out[after])) after++
    const empty = out[after] === '}'
    out = `${out.slice(0, open + 1)}\n${indent}${JSON.stringify(key)}: ${body}${empty ? '' : ','}${out.slice(open + 1)}`
  }
  return out
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const check = process.argv.includes('--check')
  const en = JSON.parse(fs.readFileSync(path.join(LOCALES, 'en.json'), 'utf8'))
  let missingFiles = 0
  for (const name of fs.readdirSync(LOCALES).filter((f) => f.endsWith('.json') && f !== 'en.json').sort()) {
    const file = path.join(LOCALES, name)
    const text = fs.readFileSync(file, 'utf8')
    const missing = missingOf(en, JSON.parse(text))
    if (!missing.length) continue
    missingFiles++
    if (check) { console.log(`${name}: ${missing.length} missing`); continue }
    const next = insertMissing(text, en)
    JSON.parse(next)
    fs.writeFileSync(file, next)
    console.log(`${name}: added ${missing.length}`)
  }
  if (check && missingFiles) process.exit(1)
}
