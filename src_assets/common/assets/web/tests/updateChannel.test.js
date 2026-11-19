import assert from 'node:assert/strict'
import test from 'node:test'
import { readFileSync } from 'node:fs'

import { resetUpdateStatus, useUpdateStatus } from '../composables/useUpdateStatus.js'

const response = (data, status = 200) => ({ ok: status >= 200 && status < 300, status, json: async () => data })
const release = (tag, prerelease = false) => ({ tag_name: tag, name: tag, body: '', html_url: `https://example.com/${tag}`, prerelease })

// a host whose configuration is kept in memory; GitHub answers with one stable and one pre-release
function host(t, { saves = true, savesNth = null, slow = false } = {}) {
  const original = globalThis.fetch
  t.after(() => { globalThis.fetch = original; resetUpdateStatus() })
  const state = { config: { version: 'v1.0.0', notify_pre_releases: false, other: 'kept' }, posts: [] }
  globalThis.fetch = async (url, options = {}) => {
    const u = String(url)
    if (u === '/api/config' && options.method === 'POST') {
      const body = JSON.parse(options.body)
      state.posts.push(body)
      if (slow) await new Promise((resolve) => setTimeout(resolve, 15))
      if (!saves || (savesNth && !savesNth(state.posts.length))) return response({ status: false, error: 'denied' })
      state.config = body
      return response({ status: true })
    }
    if (u === '/api/config') return response(structuredClone(state.config))
    if (u.endsWith('/latest')) return response(release('v2.0.0'))
    return response([release('v2.1.0-beta', true), release('v2.0.0')])
  }
  return state
}

test('更新通道 of 关于 saves notify_pre_releases on the host (the rest of the configuration as it was) and checks again with it', async (t) => {
  const state = host(t)
  const update = useUpdateStatus()
  await update.check()
  assert.equal(update.notifyPreReleases.value, false)
  assert.equal(await update.setPreReleases(true), true)
  assert.equal(state.posts.length, 1)
  assert.equal(state.posts[0].notify_pre_releases, true)
  assert.equal(state.posts[0].other, 'kept')
  assert.equal(update.notifyPreReleases.value, true)
  assert.equal(update.state.value, 'available')
  assert.equal(await update.setPreReleases(false), true)
  assert.equal(state.config.notify_pre_releases, false)
  assert.equal(update.notifyPreReleases.value, false)
})

test('a channel the host did not save shows the old choice again', async (t) => {
  host(t, { saves: false })
  const update = useUpdateStatus()
  await update.check()
  assert.equal(await update.setPreReleases(true), false)
  assert.equal(update.notifyPreReleases.value, false)
})

test('saves of the channel run one after another, in the order asked', async (t) => {
  const state = host(t, { slow: true })
  const update = useUpdateStatus()
  await update.check()
  const first = update.setPreReleases(true)
  const second = update.setPreReleases(false)
  assert.equal(update.notifyPreReleases.value, false, 'the page shows the last choice at once')
  assert.deepEqual(await Promise.all([first, second]), [true, true])
  assert.deepEqual(state.posts.map((p) => p.notify_pre_releases), [true, false], 'one save at a time, the later one last')
  assert.equal(state.config.notify_pre_releases, false)
  assert.equal(update.notifyPreReleases.value, false)
})

test('a failed save goes back to what the host saved last, not to what the page showed when the call began', async (t) => {
  const state = host(t, { slow: true, savesNth: (n) => n === 1 })
  const update = useUpdateStatus()
  await update.check()
  const first = update.setPreReleases(true)
  const second = update.setPreReleases(false)
  assert.deepEqual(await Promise.all([first, second]), [true, false])
  assert.equal(state.config.notify_pre_releases, true, 'the host kept the first save')
  assert.equal(update.notifyPreReleases.value, true, 'the failed second save shows what the host saved last (true)')
})

test('关于 passes the channel to the page and opens the update sheet when its check finds an update', () => {
  const about = readFileSync(new URL('../views/About.vue', import.meta.url), 'utf8')
  assert.match(about, /:update-channel="updateChannel"/)
  assert.match(about, /@channel-change="onChannel"/)
  assert.match(about, /update\.setPreReleases\(value === 'prerelease'\)/)
  assert.match(about, /async function onCheck\(\) \{\s*await update\.check\(\)\s*if \(update\.state\.value === 'available'\) sheet\.value = true/)
})
