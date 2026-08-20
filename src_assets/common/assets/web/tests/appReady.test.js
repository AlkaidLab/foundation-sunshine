import test from 'node:test'
import assert from 'node:assert/strict'
import { markWebUiReady, WEBUI_READY_MESSAGE } from '../utils/appReady.js'

const createDocument = () => ({ documentElement: { dataset: {} } })

test('marks a standalone WebUI as ready without messaging a parent', () => {
  const documentObject = createDocument()
  const windowObject = {}
  windowObject.parent = windowObject

  markWebUiReady(windowObject, documentObject)

  assert.equal(documentObject.documentElement.dataset.sunshineReady, 'true')
})

test('notifies the parent when the WebUI is embedded by Tauri', () => {
  const documentObject = createDocument()
  const messages = []
  const windowObject = {
    isTauri: true,
    parent: {
      postMessage: (...args) => messages.push(args),
    },
  }

  markWebUiReady(windowObject, documentObject)

  assert.deepEqual(messages, [[{ type: WEBUI_READY_MESSAGE }, '*']])
})
