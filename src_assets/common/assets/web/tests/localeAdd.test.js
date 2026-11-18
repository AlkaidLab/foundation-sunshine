import test from 'node:test'
import assert from 'node:assert/strict'
import { insertMissing, missingOf } from '../../../../../scripts/add-missing-locale-keys.mjs'

const en = { a: { x: 'X', y: 'Y', deep: { p: 'P', q: 'Q' } }, b: 'B', c: { n: { m: 'M' } } }

test('missing keys are found at the highest level they are missing', () => {
  const have = { a: { x: 'x', deep: { p: 'p' } }, b: 'b' }
  assert.deepEqual(missingOf(en, have), [{ parent: ['a'], key: 'y', value: 'Y' }, { parent: ['a', 'deep'], key: 'q', value: 'Q' }, { parent: [], key: 'c', value: { n: { m: 'M' } } }])
})

test('only new lines are added: every line that was in the file is still there, unchanged and in order', () => {
  const text = '{\n  "b": "bb",\n  "a": {\n    "x": "xx",\n    "deep": {\n      "p": "pp"\n    }\n  }\n}\n'
  const next = insertMissing(text, en)
  const before = text.split('\n')
  const after = next.split('\n')
  let at = 0
  for (const line of before) { const hit = after.indexOf(line, at); assert.ok(hit >= at, `line lost or changed: ${line}`); at = hit + 1 }
  const tree = JSON.parse(next)
  assert.equal(tree.a.y, 'Y'); assert.equal(tree.a.deep.q, 'Q'); assert.equal(tree.c.n.m, 'M')
  assert.equal(tree.b, 'bb'); assert.equal(tree.a.x, 'xx'); assert.equal(tree.a.deep.p, 'pp')
  assert.deepEqual(missingOf(en, tree), [])
  assert.equal(after.length - before.length, next.split('\n').length - before.length)
})

test('an object that is empty gets its first key without a stray comma', () => {
  const next = insertMissing('{\n  "a": {}\n}\n', { a: { x: 'X' } })
  assert.deepEqual(JSON.parse(next), { a: { x: 'X' } })
})
