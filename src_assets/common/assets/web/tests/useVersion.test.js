import assert from 'node:assert/strict'
import test from 'node:test'

import { VERSION_CHECK_STATUS, useVersion } from '../composables/useVersion.js'

const createRelease = (tagName, prerelease = false) => ({
  tag_name: tagName,
  name: tagName,
  body: '',
  html_url: `https://example.com/${tagName}`,
  prerelease,
})

const createResponse = (data, status = 200) => ({
  ok: status >= 200 && status < 300,
  status,
  json: async () => data,
})

test('version check reports ready and skips prerelease requests when disabled', async (t) => {
  const originalFetch = globalThis.fetch
  const requests = []
  t.after(() => {
    globalThis.fetch = originalFetch
  })

  globalThis.fetch = async (url) => {
    requests.push(String(url))
    return createResponse(createRelease('v2.0.0'))
  }

  const state = useVersion()
  await state.fetchVersions({ version: 'v1.0.0', notify_pre_releases: false })

  assert.equal(state.versionCheckStatus.value, VERSION_CHECK_STATUS.READY)
  assert.equal(state.loading.value, false)
  assert.equal(state.stableBuildAvailable.value, true)
  assert.equal(requests.length, 1)
  assert.match(requests[0], /releases\/latest$/)
})

test('version check loads prereleases only when subscribed', async (t) => {
  const originalFetch = globalThis.fetch
  const requests = []
  t.after(() => {
    globalThis.fetch = originalFetch
  })

  globalThis.fetch = async (url) => {
    requests.push(String(url))
    if (String(url).endsWith('/latest')) return createResponse(createRelease('v1.0.0'))
    return createResponse([createRelease('v1.1.0', true)])
  }

  const state = useVersion()
  await state.fetchVersions({ version: 'v1.0.0', notify_pre_releases: 'true' })

  assert.equal(state.versionCheckStatus.value, VERSION_CHECK_STATUS.READY)
  assert.equal(state.preReleaseBuildAvailable.value, true)
  assert.equal(requests.length, 2)
})

test('version check reports an error instead of treating a failed request as latest', async (t) => {
  const originalFetch = globalThis.fetch
  const originalConsoleError = console.error
  t.after(() => {
    globalThis.fetch = originalFetch
    console.error = originalConsoleError
  })

  globalThis.fetch = async () => createResponse(null, 503)
  console.error = () => {}

  const state = useVersion()
  await state.fetchVersions({ version: 'v1.0.0', notify_pre_releases: false })

  assert.equal(state.versionCheckStatus.value, VERSION_CHECK_STATUS.ERROR)
  assert.equal(state.loading.value, false)
  assert.equal(state.stableBuildAvailable.value, false)
})
