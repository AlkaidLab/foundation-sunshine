import test from 'node:test'
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'

// The developers' answers to the feedback the person sent come back to this page: the shell asks for them (the library's useFeedbackReplies) and puts each new one into the status button of the caption bar;
// the sheet that shows an answer is drawn by DevReportHost, which the shell already mounts for the feedback panel. A page embedded in the Panel leaves it to the Panel.
const shell = await readFile(new URL('../views/WebShell.vue', import.meta.url), 'utf8')

test('the shell asks for the answers once, next to the notices that share the status button, and not inside the Panel', () => {
  assert.match(shell, /import \{[^}]*useFeedbackReplies[^}]*\} from 'foundation-ui\/vue'/)
  assert.equal((shell.match(/useFeedbackReplies\(/g) || []).length, 1, 'asked from one place')
  assert.match(shell, /if \(!embedded\) useFeedbackReplies\(\{ locale: uiLocale \}\)/)
  assert.ok(shell.indexOf('useSystemNotices({') < shell.indexOf('useFeedbackReplies({'), 'after the component notices')
  assert.match(shell, /<DevReportHost v-if="!embedded" :locale="String\(uiLocale\)" \/>/, 'the sheet of an answer is drawn by the host of the feedback panel')
  assert.match(shell, /<MStatusBell /, 'the bell is on this page')
})
