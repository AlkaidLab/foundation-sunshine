import assert from 'node:assert/strict'
import fs from 'node:fs'
import test from 'node:test'

const shell = fs.readFileSync(new URL('../views/WebShell.vue', import.meta.url), 'utf8')

test('the Web UI treats a valid HTTP response, including temporary 503, as reachability', () => {
  assert.match(shell, /response\.status >= 100 && response\.status < 600/)
  assert.doesNotMatch(shell, /response\.status !== 502|response\.status !== 503/)
  assert.match(shell, /useHostConnection\(\{[\s\S]*consecutive failed probes before offline/)
})
