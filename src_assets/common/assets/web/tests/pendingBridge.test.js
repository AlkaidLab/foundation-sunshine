import test from 'node:test'
import assert from 'node:assert/strict'
import { nextTick } from 'vue'
import { createPendingStore, exposePending, mountPendingBridge } from 'foundation-ui/pending'
import { postPendingMessage } from '../composables/pendingBridge.js'

test('embedded application order save returns a cloneable reply and supports undo', async () => {
  const frame = createPendingStore()
  const parent = createPendingStore()
  const writes = []
  let onFrame = () => {}
  let onParent = () => {}
  const parentWindow = { postMessage(message) { const copy = structuredClone(message); queueMicrotask(() => onParent(copy)) } }
  frame.define('apps', {
    page: 'apps',
    save: async (entries) => { writes.push(entries[0].after.slice()); return { ok: true } },
    revertSaved: async (entries) => { writes.push(entries[0].before.slice()); return { ok: true } },
  })
  const stopFrame = exposePending(frame, {
    post: (message) => postPendingMessage(parentWindow, message),
    listen: (fn) => { onFrame = fn; return () => {} },
  })
  const stopParent = mountPendingBridge(parent, {
    post: (message) => { const copy = structuredClone(message); queueMicrotask(() => onFrame(copy)) },
    listen: (fn) => { onParent = fn; return () => {} },
    timeout: 100,
  })
  try {
    frame.sync('apps', [{ key: 'order', before: ['Desktop', 'Steam'], after: ['Steam', 'Desktop'] }])
    await nextTick()
    await new Promise((resolve) => setTimeout(resolve, 0))
    await parent.request()
    assert.equal((await parent.confirm()).ok, true)
    assert.equal(parent.count.value, 0)
    assert.equal(frame.count.value, 0)
    assert.equal((await parent.undoLast()).ok, true)
    assert.deepEqual(writes, [['Steam', 'Desktop'], ['Desktop', 'Steam']])
  } finally {
    stopFrame()
    stopParent()
  }
})
