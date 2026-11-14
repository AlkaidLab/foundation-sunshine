import test from 'node:test'
import assert from 'node:assert/strict'
import { isSaveCommandUnavailable, saveThroughDialog } from '../utils/saveDialog.js'

test('a save dialog answer is passed through', async () => {
  assert.equal(await saveThroughDialog(async (command, args) => `${command}:${args.defaultName}`, 'save_text_file', { defaultName: 'sunshine.conf' }), 'save_text_file:sunshine.conf')
})

test('closing the save dialog is a quiet cancel, not an error', async () => {
  assert.equal(await saveThroughDialog(async () => { throw 'cancelled' }, 'save_text_file', {}), null)

  const aborted = Object.assign(new Error('The user aborted a request.'), { name: 'AbortError' })
  assert.equal(await saveThroughDialog(async () => { throw aborted }, 'save_text_file', {}), null)
})

test('a real save failure keeps its reason', async () => {
  await assert.rejects(
    saveThroughDialog(async () => { throw 'write failed: disk full' }, 'save_text_file', {}),
    (error) => error === 'write failed: disk full',
  )
})

test('only a missing control panel command falls back to the browser download', () => {
  assert.equal(isSaveCommandUnavailable('Command save_text_file not found'), true)
  assert.equal(isSaveCommandUnavailable('Command save_binary_file not allowed by ACL'), true)
  assert.equal(isSaveCommandUnavailable('save_binary_file not allowed on origin [tauri://localhost]'), true)
  assert.equal(isSaveCommandUnavailable('write failed: file not found'), false)
  assert.equal(isSaveCommandUnavailable('write failed: disk full'), false)
  assert.equal(isSaveCommandUnavailable('cancelled'), false)
})
