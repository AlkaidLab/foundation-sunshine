import test from 'node:test'
import assert from 'node:assert/strict'
import { readdirSync, readFileSync, statSync } from 'node:fs'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = join(dirname(fileURLToPath(import.meta.url)), '..')
const walk = (dir) => readdirSync(dir).flatMap((n) => (statSync(join(dir, n)).isDirectory() ? walk(join(dir, n)) : n.endsWith('.vue') ? [join(dir, n)] : []))

test('the settings pages show a text or number setting as a row with a dialog, not as an outlined field boxed in a row', () => {
  for (const file of [...walk(join(root, 'configs/tabs')), ...walk(join(root, 'components/cfg'))]) {
    assert.doesNotMatch(readFileSync(file, 'utf8'), /<MFieldItem|<md-outlined-text-field|m-item--field/, file)
  }
})

test('CfgField and CfgPath are rows that open the dialog', () => {
  assert.match(readFileSync(join(root, 'components/cfg/CfgField.vue'), 'utf8'), /<MValueItem/)
  assert.match(readFileSync(join(root, 'components/cfg/CfgPath.vue'), 'utf8'), /<MValueItem/)
})
