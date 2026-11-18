import assert from 'node:assert/strict'
import fs from 'node:fs'
import test from 'node:test'

// 系统 › 网络概览 of the Web UI: the same foundation-ui page as the control panel, two steps from the rail (系统, then the row)
const shell = fs.readFileSync(new URL('../views/WebShell.vue', import.meta.url), 'utf8')
const page = fs.readFileSync(new URL('../views/NetworkOverview.vue', import.meta.url), 'utf8')

test('网络概览 is a row of the 系统 list behind #network-overview, with the shared row line', () => {
  assert.match(shell, /network: \{ rail: 'system', path: null, hash: 'network-overview', icon: 'lan', tone: 'primary', text: \(locale\) => networkOverviewTexts\(locale\)\.title \}/)
  assert.match(shell, /network: lazy\(\(\) => import\('\.\/NetworkOverview\.vue'\)\)/)
  assert.match(shell, /ROW_OF_PAGE = \{[^}]*network: 'network'/)
  const order = shell.indexOf("components: { rail: 'system'") < shell.indexOf("network: { rail: 'system'") && shell.indexOf("network: { rail: 'system'") < shell.indexOf("troubleshooting: { rail: 'system'")
  assert.ok(order, 'between 组件状态 and 日志与诊断, as in the control panel')
})

test('the page reads the shared state: opening reads the host cache, 重新检测 asks again, a fix opens 串流设置 › 网络', () => {
  assert.match(page, /createNetworkOverview\(\{/)
  assert.match(page, /@recheck="state\.refresh\(true\)"/)
  assert.match(page, /webShell\?\.openConfig\(\{ tab: 'network' \}\)/)
  assert.ok(!/setInterval|setTimeout/.test(page), 'nothing asks the host on a timer')
})
