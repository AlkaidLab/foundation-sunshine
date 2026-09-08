import assert from 'node:assert/strict'
import test from 'node:test'

import { resolveDialogTeleportTarget } from '../utils/focus.js'

test('dialog overlays stay inside the Bootstrap modal that opened them', () => {
  const modal = { id: 'editAppModal' }
  const trigger = {
    closest(selector) {
      assert.equal(selector, '.modal')
      return modal
    },
  }

  assert.equal(resolveDialogTeleportTarget(trigger), modal)
})

test('standalone dialog overlays fall back to the document body target', () => {
  const trigger = { closest: () => null }

  assert.equal(resolveDialogTeleportTarget(trigger), 'body')
  assert.equal(resolveDialogTeleportTarget(null), 'body')
})
