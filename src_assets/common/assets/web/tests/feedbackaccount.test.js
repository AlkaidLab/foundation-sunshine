import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

const read = (p) => fs.readFileSync(new URL(p, import.meta.url), 'utf8')

test('帮助与关于 of the Web UI has 账号与反馈 and the report sheet host without developer mode', () => {
  const about = read('../views/About.vue')
  assert.match(about, /<MFeedbackAccount :locale="String\(locale\)" \/>/)
  assert.ok(!/MFeedbackAccount[^\n]*developer/i.test(about))
  const shell = read('../views/WebShell.vue')
  assert.match(shell, /<DevReportHost v-if="!embedded"/)
  assert.match(shell, /reportHere: \(\{ route, place, text \}\) => \{ void openDevReport\(/)
})
test('the pairing PIN, the QR and the device names are marked for the screenshot redaction', () => {
  const pin = read('../views/Pin.vue')
  for (const kind of ['client', 'pin']) assert.match(pin, new RegExp(`data-redact="${kind}"`))
  assert.ok((pin.match(/data-redact="pin"/g) || []).length >= 3, 'the PIN pad, the QR image and the QR PIN')
})

test('the embedded Web UI answers the page around it with the rectangles of its marked elements', () => {
  const shell = read('../views/WebShell.vue')
  assert.match(shell, /if \(embedded\) \{ answerRedactRequests\(\); return \}/)
  assert.match(shell, /import \{ answerRedactRequests,/)
})

test('the file rows of the Files tab are settings rows by key, so the paths are marked by data-cfg', () => {
  const files = read('../configs/tabs/Files.vue')
  for (const key of ['file_apps', 'credentials_file', 'log_path']) assert.match(files, new RegExp(`data-cfg="${key}"`))
})
