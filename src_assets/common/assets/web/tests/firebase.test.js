import test, { after } from 'node:test'
import assert from 'node:assert/strict'

const originalRequestIdleCallback = globalThis.requestIdleCallback
globalThis.requestIdleCallback = () => 1

const { trackEvent } = await import('../config/firebase.js')

after(() => {
  if (originalRequestIdleCallback) {
    globalThis.requestIdleCallback = originalRequestIdleCallback
  } else {
    delete globalThis.requestIdleCallback
  }
})

test('accepts explicitly null analytics parameters', () => {
  assert.doesNotThrow(() => trackEvent('null_params', null))
})

test('does not throw when an analytics parameter contains a cycle', () => {
  const circularValue = {}
  circularValue.self = circularValue

  assert.doesNotThrow(() => trackEvent('circular_params', { value: circularValue }))
})
