import test from 'node:test'
import assert from 'node:assert/strict'
import {
  installExternalLinkHandler,
  isAllowedExternalUrl,
  openExternalUrl,
} from '../utils/helpers.js'
import { LEGAL_RESOURCES } from '../config/resources.js'

test('legal resources target the current Foundation repository', () => {
  const resources = Object.fromEntries(LEGAL_RESOURCES.map((resource) => [resource.id, resource.href]))

  assert.equal(
    resources.license,
    'https://github.com/AlkaidLab/foundation-sunshine/blob/master/LICENSE',
  )
  assert.equal(
    resources['third-party-notice'],
    'https://github.com/AlkaidLab/foundation-sunshine/blob/master/NOTICE',
  )
})

test('external URL allowlist matches the control panel command', () => {
  assert.equal(isAllowedExternalUrl('https://www.alkaidlab.com/'), true)
  assert.equal(isAllowedExternalUrl('http://127.0.0.1/'), true)
  assert.equal(isAllowedExternalUrl('ms-windows-store://pdp/?productid=example'), true)
  assert.equal(isAllowedExternalUrl('file:///C:/Windows/System32'), false)
  assert.equal(isAllowedExternalUrl('javascript:alert(1)'), false)
})

test('opens external URLs through the current Tauri command', async () => {
  const calls = []
  const windowObject = {
    isTauri: true,
    __TAURI__: {
      core: {
        invoke: async (...args) => {
          calls.push(args)
          return true
        },
      },
    },
  }

  assert.equal(await openExternalUrl('https://www.alkaidlab.com/', windowObject), true)
  assert.deepEqual(calls, [['open_external_url', { url: 'https://www.alkaidlab.com/' }]])
})

test('rejects unsupported URLs before invoking Tauri', async () => {
  let invoked = false
  const windowObject = {
    isTauri: true,
    __TAURI__: {
      core: {
        invoke: async () => {
          invoked = true
          return true
        },
      },
    },
  }

  await assert.rejects(openExternalUrl('file:///C:/Windows/System32', windowObject))
  assert.equal(invoked, false)
})

test('routes external anchor clicks through the Tauri command', async () => {
  let clickHandler
  let prevented = false
  const calls = []
  const anchor = {
    href: 'https://www.alkaidlab.com/',
    hasAttribute: () => false,
  }
  const documentObject = {
    addEventListener: (type, handler) => {
      if (type === 'click') clickHandler = handler
    },
    removeEventListener: () => {},
  }
  const windowObject = {
    isTauri: true,
    location: { origin: 'https://127.0.0.1:47990' },
    __TAURI__: {
      core: {
        invoke: async (...args) => {
          calls.push(args)
          return true
        },
      },
    },
  }

  const dispose = installExternalLinkHandler(documentObject, windowObject)
  clickHandler({
    button: 0,
    defaultPrevented: false,
    target: { closest: () => anchor },
    preventDefault: () => {
      prevented = true
    },
  })
  await Promise.resolve()

  assert.equal(prevented, true)
  assert.deepEqual(calls, [['open_external_url', { url: 'https://www.alkaidlab.com/' }]])
  dispose()
})

test('leaves same-origin navigation to the WebUI', () => {
  let clickHandler
  let prevented = false
  const documentObject = {
    addEventListener: (type, handler) => {
      if (type === 'click') clickHandler = handler
    },
    removeEventListener: () => {},
  }
  const windowObject = {
    isTauri: true,
    location: { origin: 'https://127.0.0.1:47990' },
    __TAURI__: { core: { invoke: async () => true } },
  }

  const dispose = installExternalLinkHandler(documentObject, windowObject)
  clickHandler({
    button: 0,
    defaultPrevented: false,
    target: {
      closest: () => ({
        href: 'https://127.0.0.1:47990/config',
        hasAttribute: () => false,
      }),
    },
    preventDefault: () => {
      prevented = true
    },
  })

  assert.equal(prevented, false)
  dispose()
})
