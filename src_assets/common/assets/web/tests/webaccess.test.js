import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const WEB = join(dirname(fileURLToPath(import.meta.url)), '..')
const read = (file) => readFileSync(join(WEB, file), 'utf8')

test('主机设置 › 网络 opens with 访问范围: the shared component, the page values, QR, the restart and the password page', () => {
  const page = read('configs/tabs/Network.vue')
  const at = page.indexOf('<MWebAccess')
  assert.ok(at > 0 && at < page.indexOf('<MReachOverview'), 'the access scope is the first section of the page')
  for (const part of ['v-model:scope="scope"', 'v-model:bind="bind"', ':listeners="listenerInfo"', ':qr="qrMatrix"', '@restart="restartAsk = true"', '@password="goPassword"']) assert.ok(page.includes(part), part)
  assert.ok(!page.includes('<CfgChoice :config="config" k="origin_web_ui_allowed"'), 'the old unlabeled row is gone')
  assert.ok(!page.includes('data-cfg="bind_address"'), 'the bind address has one place')
  assert.ok(page.includes("'origin_web_ui_allowed'") && page.includes('activeListener.value[key] !== undefined'), 'a changed scope shows the restart notice')
})

test('the guide step saves origin_web_ui_allowed and bind_address through the host\'s own configuration route', () => {
  const guide = read('composables/useGuide.js')
  assert.match(guide, /async function loadWebAccess/)
  assert.match(guide, /async function saveWebAccess\(extra\)[\s\S]*saveSetupChoices\(\{ completed: state\.setup\.completed, extra \}\)/)
  assert.match(guide, /loadWebAccess,\s*saveWebAccess,/)
})

test('the page says 访问范围 in Chinese and the three choices are the host\'s values', () => {
  const zh = JSON.parse(read('public/assets/locale/zh.json'))
  assert.equal(zh.config.origin_web_ui_allowed, '访问范围')
  for (const value of ['pc', 'lan', 'wan']) assert.ok(zh.config[`origin_web_ui_allowed_${value}`])
})
