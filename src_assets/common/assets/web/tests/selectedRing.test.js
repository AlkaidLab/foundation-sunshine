// One selected-state rule: a selected card or choice is its container colour plus a check, never a border, outline or box-shadow ring wider than 1 px.
// The scanner is foundation-ui's (the same one its own test runs over the package); the focus ring (:focus-visible) is not a selected state.
import test from 'node:test'
import assert from 'node:assert/strict'
import { fileURLToPath } from 'node:url'
import { selectedRingsIn } from 'foundation-ui/testing/selected-ring'

test('no selected-state rule of the Web UI draws a ring wider than 1 px', () => {
  const found = selectedRingsIn(fileURLToPath(new URL('..', import.meta.url)))
  assert.deepEqual(found.map((f) => `${f.file.split('/web/')[1]}:${f.line} ${f.selector} { ${f.rings.join('; ')} }`), [])
})
