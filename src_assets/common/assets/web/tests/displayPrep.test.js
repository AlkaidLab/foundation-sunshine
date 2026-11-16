import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'
import { PREP_BASIC, prepChoices } from '../composables/displayPrep.js'

const WEB = join(dirname(fileURLToPath(import.meta.url)), '..')

test('扩展显示器 is offered on Windows whenever the base virtual display is the one that is streamed', () => {
  assert.deepEqual(prepChoices({ windows: true, virtual: true, current: 'ensure_only_display' }), [...PREP_BASIC, 'ensure_secondary'])
  assert.deepEqual(prepChoices({ windows: true, virtual: true }), [...PREP_BASIC, 'ensure_secondary'])
})

test('it is not offered for a physical display or off Windows, where the host does not support it', () => {
  assert.deepEqual(prepChoices({ windows: true, virtual: false, current: 'ensure_only_display' }), PREP_BASIC)
  assert.deepEqual(prepChoices({ windows: false, virtual: true }), PREP_BASIC)
})

test('a saved 扩展显示器 is never hidden', () => {
  assert.deepEqual(prepChoices({ windows: true, virtual: false, current: 'ensure_secondary' }), [...PREP_BASIC, 'ensure_secondary'])
})

test('the display page draws the choices from this list', () => {
  const page = readFileSync(join(WEB, 'configs/tabs/display/DisplayOutput.vue'), 'utf8')
  assert.match(page, /prepChoices\(\{ windows: windows\.value, virtual: chosen\.value === 'virtual', current: props\.config\.display_device_prep \}\)/)
  assert.match(page, /:options="prepOptions"/)
  assert.doesNotMatch(page, /prepExtra/)
})

test('the host configuration answer carries the Windows build for the first-run guide', () => {
  const root = join(WEB, '..', '..', '..', '..')
  const answer = readFileSync(join(root, 'src/confighttp/configuration.cpp'), 'utf8')
  assert.match(answer, /#include "src\/doctor\/doctor_os\.h"/)
  assert.match(answer, /if \(const auto windows_build = doctor::os::windows_build\(\)\) outputTree\.put\("windows_build", \*windows_build\);/)
})
