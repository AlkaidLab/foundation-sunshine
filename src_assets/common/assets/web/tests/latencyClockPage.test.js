import test from 'node:test'
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'

// 延迟测试时钟 is a page of the help rail of the Web UI (#latency-clock), the shared clock of foundation-ui
const read = (p) => readFile(new URL(p, import.meta.url), 'utf8')

test('the latency clock is a page of the system rail (the toolbox of the panel) with its own address and the shared clock', async () => {
  const shell = await read('../views/WebShell.vue')
  assert.match(shell, /'latency-clock': lazy\(\(\) => import\('\.\/LatencyClock\.vue'\)\)/)
  assert.match(shell, /'latency-clock': \{ rail: 'system', path: null, hash: 'latency-clock', icon: 'speed', tone: 'tertiary', text: \(locale\) => latencyClockTexts\(locale\)\.title \}/)
  const page = await read('../views/LatencyClock.vue')
  assert.match(page, /<MLatencyClock :locale="String\(locale\)" @open-external="openExternalUrl" \/>/)
})

test('the test tools are a page of the system rail too, next to the latency clock', async () => {
  const shell = await read('../views/WebShell.vue')
  assert.match(shell, /'test-tools': lazy\(\(\) => import\('\.\/TestTools\.vue'\)\)/)
  assert.match(shell, /'test-tools': \{ rail: 'system', path: null, hash: 'test-tools', icon: 'science', tone: 'secondary', text: \(locale\) => testToolTexts\(locale\)\.title \}/)
  assert.match(await read('../views/TestTools.vue'), /<MTestTools :locale="String\(locale\)"[^>]*\/>/)
})
