import './helpers/foundationUi.mjs'
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'

const { startupProblems } = await import('foundation-ui/diagnostics/startup.js')

test('the host log of a startup with the driver missing gives one short localized line and keeps the whole message', () => {
  const log = ['Fatal: ViGEmBus is not installed or running. You must install ViGEmBus for gamepad support! (If you do not need gamepad support, you can ignore this message)']
  const [problem] = startupProblems(log, 'zh')
  assert.equal(problem.short, '未安装 ViGEmBus，手柄不可用')
  assert.ok(problem.raw.includes('You must install ViGEmBus') && problem.raw.includes('ignore this message'))
  assert.equal(problem.known.go, 'drivers')
})

test('the overview shows the startup problems as the compact card and no longer as a block of raw lines', async () => {
  const home = await readFile(new URL('../views/Home.vue', import.meta.url), 'utf8')
  assert.match(home, /<MStartupProblems /)
  assert.ok(!/home-fatal/.test(home), 'the old block of raw lines is gone')
  assert.match(home, /@logs="go\('troubleshooting'\)"/)
})
