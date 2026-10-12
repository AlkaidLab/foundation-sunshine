import test from 'node:test'
import assert from 'node:assert/strict'
import { nextTick } from 'vue'
import { useTroubleshooting } from '../composables/useTroubleshooting.js'

const liveResponse = (text, size, range = 'full') => ({
  ok: true,
  status: 200,
  headers: new Map([
    ['X-Log-Size', String(size)],
    ['X-Log-Range', range],
  ]),
  text: async () => text,
})

test('troubleshooting live view caps memory and rendered lines without losing the file offset', async () => {
  const originalDocument = globalThis.document
  const originalWarn = console.warn
  globalThis.document = {
    addEventListener() {},
    removeEventListener() {},
  }
  console.warn = () => {}

  let calls = 0
  let resolveFirstRefresh
  const state = useTroubleshooting({
    fetchLogs: () => {
      calls += 1
      return new Promise((resolve) => {
        resolveFirstRefresh = (response) => resolve(response)
      })
    },
  })

  try {
    const firstRefresh = state.refreshLogs()
    const overlappingRefresh = state.refreshLogs()
    await overlappingRefresh
    assert.equal(calls, 1)

    const largeLog = Array.from({ length: 2600 }, (_, index) => 'line-' + index + '-' + 'x'.repeat(96)).join('\n')
    resolveFirstRefresh(liveResponse(largeLog, largeLog.length))
    await firstRefresh
    await nextTick()

    assert.equal(state.logOffset.value, largeLog.length)
    assert.ok(state.logs.value.length <= 256 * 1024)
    assert.equal(state.actualLogs.value.split('\n').length, 2000)
    assert.ok(state.actualLogs.value.includes('line-2599'))
  } finally {
    console.warn = originalWarn
    globalThis.document = originalDocument
  }
})

test('troubleshooting incremental updates append to the capped live view', async () => {
  const originalDocument = globalThis.document
  const originalWarn = console.warn
  globalThis.document = {
    addEventListener() {},
    removeEventListener() {},
  }
  console.warn = () => {}

  const state = useTroubleshooting({
    fetchLogs: async (offset) => liveResponse(offset === 500000 ? '\nnew-line' : 'old-lines', 500010, 'incremental'),
  })

  try {
    state.logs.value = 'old-lines'
    state.logOffset.value = 500000
    await state.refreshLogs()
    await nextTick()

    assert.equal(state.logs.value, 'old-lines\nnew-line')
    assert.equal(state.logOffset.value, 500010)
    assert.ok(state.actualLogs.value.includes('new-line'))
  } finally {
    console.warn = originalWarn
    globalThis.document = originalDocument
  }
})

test('troubleshooting keeps 2000 actual lines when logs end with a newline', () => {
  const originalDocument = globalThis.document
  const originalWarn = console.warn
  globalThis.document = { addEventListener() {}, removeEventListener() {} }
  console.warn = () => {}

  try {
    const state = useTroubleshooting()
    state.logs.value = Array.from({ length: 2000 }, (_, index) => 'line-' + index).join('\n') + '\n'

    const renderedLines = state.actualLogs.value.split('\n')
    assert.equal(renderedLines.length, 2000)
    assert.equal(renderedLines[0], 'line-0')
    assert.equal(renderedLines.at(-1), 'line-1999')
  } finally {
    console.warn = originalWarn
    globalThis.document = originalDocument
  }
})

test('troubleshooting searches the full retained cache before limiting matching lines', () => {
  const originalDocument = globalThis.document
  const originalWarn = console.warn
  globalThis.document = { addEventListener() {}, removeEventListener() {} }
  console.warn = () => {}

  try {
    const state = useTroubleshooting()
    state.logs.value = ['early-match', ...Array.from({ length: 2000 }, (_, index) => 'line-' + index)].join('\n')
    state.logFilter.value = 'early-match'

    assert.equal(state.actualLogs.value, 'early-match')
  } finally {
    console.warn = originalWarn
    globalThis.document = originalDocument
  }
})
