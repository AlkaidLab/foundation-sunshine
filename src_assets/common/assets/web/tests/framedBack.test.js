import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import { createFramedBack } from '../utils/framedBack.js'

const shell = fs.readFileSync(new URL('../views/WebShell.vue', import.meta.url), 'utf8')
const make = () => {
  const sent = []; const timers = []; const parent = {}
  const fb = createFramedBack({ post: (m) => sent.push(m), parent, setTimer: (fn) => { timers.push(fn); return timers.length }, clearTimer: (id) => { if (id) timers[id - 1] = null } })
  const flush = () => { for (const fn of timers.splice(0)) fn?.() }
  return { fb, sent, parent, flush }
}

test('the panel says when Back applies; only the parent window is listened to', () => {
  const { fb, parent } = make()
  assert.equal(fb.scene.canBack.value, false)
  fb.onMessage({ source: {}, data: { type: 'foundation-panel-back', show: true } })
  assert.equal(fb.scene.canBack.value, false, 'another window cannot switch Back on')
  fb.onMessage({ source: parent, data: { type: 'foundation-panel-back', show: 'yes' } })
  assert.equal(fb.scene.canBack.value, false, 'only a boolean')
  fb.onMessage({ source: parent, data: { type: 'foundation-panel-back', show: true } })
  assert.equal(fb.scene.canBack.value, true)
  fb.onMessage({ source: parent, data: { type: 'foundation-panel-back', show: false } })
  assert.equal(fb.scene.canBack.value, false)
})

test('a press of the arrow is the panel\'s: one message, no history of the frame', () => {
  const { fb, sent } = make()
  fb.scene.back()
  assert.deepEqual(sent, [{ type: 'foundation-back-click' }])
})

test('a page that is there says that it drew the arrow, once; a swap of the view does not flicker the answer', () => {
  const { fb, sent, flush } = make()
  fb.scene.pageMounted()
  assert.deepEqual(sent, [{ type: 'foundation-back-inline', drawn: true }])
  fb.scene.pageUnmounted(); fb.scene.pageMounted()
  flush()
  assert.equal(sent.length, 1, 'unmount then mount in the same moment: still one answer')
})

test('a document without any page (a loading chunk, a page with no title row) answers "not drawn" after it settled, so the panel keeps its bar', () => {
  const { fb, sent, flush } = make()
  fb.start()
  assert.deepEqual(sent, [{ type: 'foundation-back-request' }])
  flush()
  assert.deepEqual(sent.slice(1), [{ type: 'foundation-back-inline', drawn: false }])
  fb.scene.pageMounted()
  assert.deepEqual(sent.at(-1), { type: 'foundation-back-inline', drawn: true })
  fb.scene.pageUnmounted(); flush()
  assert.deepEqual(sent.at(-1), { type: 'foundation-back-inline', drawn: false })
})

test('WebShell: only an embedded shell provides the scene, listens, and asks for the state once mounted', () => {
  assert.match(shell, /provide\('m-scene', framedBack\.scene\)/)
  assert.match(shell, /window\.addEventListener\('message', framedBack\.onMessage\)/)
  assert.match(shell, /onMounted\(framedBack\.start\)/)
  const block = shell.slice(shell.indexOf('const framedBack'), shell.indexOf('const framedBack') + 400)
  assert.ok(shell.lastIndexOf('if (embedded) {', shell.indexOf('const framedBack')) > shell.indexOf('const FRAMED_IDS'), 'inside the embedded block ' + block.length)
})
