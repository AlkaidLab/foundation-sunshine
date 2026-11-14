import './helpers/foundationUi.mjs'
import assert from 'node:assert/strict'
import test from 'node:test'

const { micIsReady } = await import('foundation-ui/vue/deviceTiles.js')

// The overview's microphone tile gets its green dot from this: the Web UI's live state has `online` and `host_streaming`.
test('the microphone tile has a dot only while the microphone is online or in use, never when it is offline', () => {
  assert.equal(micIsReady({ online: false, host_streaming: false }), false)
  assert.equal(micIsReady({ online: true, host_streaming: false }), true)
  assert.equal(micIsReady({ online: false, host_streaming: true }), true)
  assert.equal(micIsReady({ state: 'idle', online: false, host_streaming: false, underruns: 0 }), false, 'what the Web UI shows as offline (the host says state idle)')
  assert.equal(micIsReady(null), false)
  assert.equal(micIsReady(undefined), false)
})
