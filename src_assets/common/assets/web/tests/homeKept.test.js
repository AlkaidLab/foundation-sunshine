import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

const read = (path) => fs.readFileSync(new URL(path, import.meta.url), 'utf8')

// The overview is drawn once: it starts from what the last visit kept and says (ctx.pending) when it cannot be drawn yet, so the tiles are not laid out from nothing and then again when the host answers.
test('the overview tells the dashboard when it cannot be drawn yet (ctx.pending) and starts from the data it kept', () => {
  const source = read('../views/Home.vue')
  assert.match(source, /const dashCtx = computed\(\(\) => \(\{ pending: pending\.value,/)
  assert.match(source, /useHostLive\(\{[^}]*keep: true/)
  assert.match(source, /hostData\.peek\('config'\)/)
  assert.match(source, /hostData\.peek\('apps'\)/)
  assert.match(source, /SETTLE_CAP_MS/)
})

test('the page does not wait for an external reading: the version check is not one of the reads the first frame needs', () => {
  const source = read('../views/Home.vue')
  const reads = /const reads = reactive\(\{([^}]*)\}\)/.exec(source)?.[1] || ''
  assert.deepEqual([...reads.matchAll(/(\w+):/g)].map((m) => m[1]).sort(), ['apps', 'config', 'logs', 'pad'])
})

test('a page that asks to keep starts from the last answers of the host and keeps its own; one that does not is unchanged', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout', 'setInterval'] })
  const { useHostLive } = await import('../composables/useHostLive.js')
  Object.defineProperty(globalThis, 'document', { value: { hidden: true, addEventListener() {}, removeEventListener() {} }, configurable: true })
  Object.defineProperty(globalThis, 'window', { value: { addEventListener() {}, removeEventListener() {} }, configurable: true })
  const options = { want: () => new Set(), platform: () => 'windows' }
  const first = useHostLive({ ...options, keep: true })
  assert.equal(first.local.value, null)
  first.sessions.value = [{ client_name: 'Phone' }]
  first.local.value = true
  const second = useHostLive({ ...options, keep: true })
  assert.deepEqual(second.sessions.value, [{ client_name: 'Phone' }])
  assert.equal(second.local.value, true)
  const plain = useHostLive(options)
  assert.deepEqual(plain.sessions.value, [])
  assert.equal(plain.local.value, null)
})
