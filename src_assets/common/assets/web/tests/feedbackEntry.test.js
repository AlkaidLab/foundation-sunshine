import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

// 反馈: a row in 帮助与关于 (the About page) and an action of the search; the form is MErrorHost of the shell
const read = (p) => fs.readFileSync(new URL(p, import.meta.url), 'utf8')

test('the About page has 账号与反馈 (with the feedback row) after 开始体检 and the search runs 反馈', () => {
  const about = read('../views/About.vue')
  assert.match(about, /doctorTexts\(String\(locale\)\)\.start[^]*<MFeedbackAccount :locale="String\(locale\)" \/>/)
  const shell = read('../views/WebShell.vue')
  assert.match(shell, /feedback: \(\) => \{ useErrors\(\)\.reportFeedback\(\) \}/)
  assert.match(shell, /<MErrorHost v-if="!embedded"/, 'an embedded frame has no form host; the row hides itself there')
})
