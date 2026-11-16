import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

const read = (p) => fs.readFileSync(new URL(p, import.meta.url), 'utf8')

test('帮助与关于 of the Web UI has the 反馈 card, and the shell has the one feedback panel, its caption button and the context menu entry, without developer mode', () => {
  const about = read('../views/About.vue')
  assert.match(about, /<MFeedbackCard :locale="String\(locale\)" \/>/)
  assert.ok(!/MFeedbackCard[^\n]*developer/i.test(about))
  assert.ok(!/MFeedbackAccount/.test(about), 'no GitHub account row: the report goes straight to the developers')
  const shell = read('../views/WebShell.vue')
  assert.match(shell, /<DevReportHost v-if="!embedded"/)
  assert.match(shell, /<MStatusBell :locale="String\(uiLocale\)" \/><MFeedbackButton :locale="String\(uiLocale\)" \/><MAnnouncementButton/, 'the feedback icon sits next to the bell')
  assert.match(shell, /reportHere: \(\{ place, text \}\) => \{ void openFeedbackHere\(\{ place, text \}\) \}/)
  assert.ok(!/ctxReport/.test(shell), 'nothing is typed into the description for the person')
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
