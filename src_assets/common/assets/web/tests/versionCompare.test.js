import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'

import SunshineVersion, { compareVersions, versionKey } from '../sunshine_version.js'

// the table shared with the host (tests/unit/test_version_compare.cpp) and the panel (src-tauri)
const vectors = JSON.parse(readFileSync(new URL('../../../../../tests/fixtures/version_vectors.json', import.meta.url), 'utf8'))

test('the shared table is loaded', () => {
  assert.equal(vectors.schema, 1)
  assert.ok(vectors.parse.length >= 25)
  assert.ok(vectors.compare.length >= 15)
})

test('versionKey matches the shared parse vectors', () => {
  for (const row of vectors.parse) assert.deepEqual(versionKey(row.input), row.key, row.input)
})

test('compareVersions and SunshineVersion match the shared compare vectors', () => {
  for (const row of vectors.compare) {
    const label = `${row.left} vs ${row.right}`
    assert.equal(compareVersions(row.left, row.right), row.order, label)
    assert.equal(compareVersions(row.right, row.left), row.order === null ? null : 0 - row.order, label)
    if (row.left && row.right) {
      const a = new SunshineVersion(null, row.left)
      assert.equal(a.compareTo(row.right), row.order, label)
      assert.equal(a.isGreater(row.right), row.order === 1, label)
      assert.equal(a.isLessThan(row.right), row.order === -1, label)
      assert.equal(a.isEqual(row.right), row.order === 0, label)
    }
  }
})

test('a release object reads its tag', () => {
  const stable = new SunshineVersion({ tag_name: 'v2026.1103.21122.杂鱼', name: 'x' }, null)
  assert.deepEqual(stable.versionParts, [2026, 1103, 21122])
  assert.equal(stable.isGreater('2026.1031.121458.杂鱼'), true)
})
