// The system rail of the Web UI is level with the control panel's: 延迟测试时钟, 测试工具 and 串流计时器 are pages, have a subtitle line and search keywords.
import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

const shell = fs.readFileSync(new URL('../views/WebShell.vue', import.meta.url), 'utf8')
const IDS = ['latency-clock', 'test-tools', 'stream-timer']

test('each of the three has a page, a rail entry and a row for its line and keywords', () => {
  for (const id of IDS) {
    assert.match(shell, new RegExp(`'${id}': lazy\\(`), `${id}: view`)
    assert.match(shell, new RegExp(`'${id}': \\{ rail: 'system'`), `${id}: rail entry`)
    assert.match(shell, new RegExp(`ROW_OF_PAGE = \\{[^}]*'${id}': '${id}'`), `${id}: row of page`)
  }
  assert.ok(fs.existsSync(new URL('../views/StreamTimer.vue', import.meta.url)))
})

test('the stream timer says its words in Chinese and English', async () => {
  const { streamTimerTexts } = await import('../views/streamTimerTexts.js')
  assert.equal(streamTimerTexts('zh-CN').title, '串流计时器'); assert.equal(streamTimerTexts('en').title, 'Stream Timer'); assert.equal(streamTimerTexts('de').title, 'Stream Timer')
  assert.deepEqual(Object.keys(streamTimerTexts('zh')), Object.keys(streamTimerTexts('en')))
})

// fs52-gamepad-test-rumble: the gamepad tool asks the host for a rumble, because a pad that exists only during a stream is not reachable from the browser
test('the test tools page hands the gamepad tool a call to the host rumble route', () => {
  const view = fs.readFileSync(new URL('../views/TestTools.vue', import.meta.url), 'utf8')
  assert.ok(view.includes(':host-rumble="hostRumble"'))
  assert.ok(view.includes("apiPostJson('/api/dualsense/rumble-test', body)") && view.includes("from '../utils/apiFetch.js'"))
})

test('the host serves the rumble route, behind the same sign-in as the tuning route, and registers the pulse with the native client', () => {
  const route = fs.readFileSync(new URL('../../../../../src/confighttp/dualsense.cpp', import.meta.url), 'utf8')
  assert.ok(route.includes('^/api/dualsense/rumble-test$') && /postRumbleTest\(resp_https_t response, req_https_t request\) \{\s*if \(!authenticate\(response, request\)\) return;/.test(route))
  const native = fs.readFileSync(new URL('../../../../../src/platform/windows/ds5/ds5_native_client.cpp', import.meta.url), 'utf8')
  assert.ok(native.includes('::ds5::rumble_test::handler().store(&test_rumble_all'))
})
