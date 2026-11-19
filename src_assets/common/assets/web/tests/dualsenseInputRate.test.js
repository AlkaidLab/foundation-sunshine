import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'

const read = (path) => readFile(new URL(path, import.meta.url), 'utf8')

test('a value above 250 Hz waits for the rate dialog; the dialog is mounted with the gamepad sub-pages', async () => {
  const store = await read('../composables/dualsenseShared.js')
  assert.match(store, /inputRateNeedsConfirm\(descriptor\(id\)\?\.key, edited\[id\], value\)\) \{ rateAsk\.value = \{ id, value \}; return \}/, 'onValue holds the value back')
  assert.match(store, /confirmRate = \(\) => \{[^}]*setValue\(ask\.id, ask\.value\)/, 'confirming applies the held value')
  assert.match(store, /cancelRate = \(\) => \{ rateAsk\.value = null \}/, 'cancelling leaves the value as it was')
  const subs = await read('../components/dualsense/DualSenseSubs.vue')
  assert.match(subs, /<InputRateDialog :show="rateAsk !== null" :option="String\(rateAsk\?\.value \?\? 'auto'\)" @cancel="cancelRate" @confirm="confirmRate" \/>/)
})

test('the rate dialog takes its text and its countdown from foundation-ui and unlocks only when the countdown is over', async () => {
  const dialog = await read('../components/dualsense/InputRateDialog.vue')
  assert.match(dialog, /import \{ inputRateWarning, INPUT_RATE_WARNING_SECONDS \} from 'foundation-ui\/settings\/index\.js'/)
  assert.match(dialog, /:disabled="!ready"/)
  assert.match(dialog, /ready && emit\('confirm'\)/, 'a click before the countdown ends does nothing')
  assert.doesNotMatch(dialog, /\$t\(/, 'no copy of its own: the text is the shared one')
})

test('resetting the rate key from 250 Hz, one key or all defaults, asks like a choice does', async () => {
  const store = await read('../composables/dualsenseShared.js')
  assert.match(store, /const resetKey = \(id\) => \{[\s\S]*?inputRateNeedsConfirm\(key\.key, edited\[id\], key\.default\)\) \{ rateAsk\.value = \{ id, value: key\.default \}; return \}/)
  assert.match(store, /const restoreDefaults = \(\) => \{[\s\S]*?inputRateNeedsConfirm\(key\.key, edited\[key\.id\], key\.default\)\) \{ rateKey = key; continue \}[\s\S]*?if \(rateKey\) rateAsk\.value = \{ id: rateKey\.id, value: rateKey\.default \}/)
})
