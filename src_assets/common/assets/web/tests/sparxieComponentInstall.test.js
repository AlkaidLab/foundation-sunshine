import test from 'node:test'
import assert from 'node:assert/strict'
import { installFromFile, installPayload, uninstallChoice } from '../utils/sparxie/componentInstall.js'

const texts = { bring: 'bring', noticeTitle: 'before', noticeYes: 'go', unverified: 'unknown', unverifiedTitle: 'title', unverifiedYes: 'install', removeText: 'cfg', removeTitle: 'rm', removeDelete: 'del', removeKeep: 'keep' }

test('a known package is sent once, an unknown one only after confirmation', async () => {
  const posts = []
  const known = await installFromFile({
    status: { noticeAck: true },
    pick: async () => 'C:/pkg.zip',
    ask: async () => { throw new Error('no ask') },
    post: async (body) => { posts.push(body); return { ok: true } },
    texts,
  })
  assert.equal(known.ok, true)
  assert.deepEqual(posts, [installPayload('C:/pkg.zip', { acknowledged: true })])
  posts.length = 0
  const asks = []
  const unknown = await installFromFile({
    status: { noticeAck: false, statement: 'injects' },
    pick: async () => 'C:/other.zip',
    ask: async (message) => { asks.push(message); return true },
    post: async (body) => { posts.push(body); return posts.length === 1 ? { ok: false, error_code: 'unverified', sha256: 'abc' } : { ok: true, verified: false } },
    texts,
  })
  assert.equal(unknown.verified, false)
  assert.deepEqual(asks, ['injects', 'unknown'])
  assert.equal(posts[1].confirm_unverified, true)
})

test('uninstall asks before deleting config.json', async () => {
  const bodies = []
  const kept = await uninstallChoice(async (body) => { bodies.push(body); return bodies.length === 1 ? { ok: false, error_code: 'confirm_config' } : { ok: true } }, async () => 'keep', texts)
  assert.equal(kept.ok, true)
  assert.deepEqual(bodies[1], { delete_config: false })
})

test('a same-size checksum mismatch asks, then installs after confirmation', async () => {
  const posts = []
  const asks = []
  const result = await installFromFile({
    status: { noticeAck: true },
    pick: async () => 'C:/changed.zip',
    ask: async (message) => { asks.push(message); return true },
    post: async (body) => { posts.push(body); return posts.length === 1 ? { ok: false, error_code: 'hash_mismatch', confirm: '未经验证的版本' } : { ok: true, verified: false, mark: '未经验证的版本' } },
    texts,
  })
  assert.equal(result.verified, false)
  assert.deepEqual(asks, ['unknown'])
  assert.equal(posts[0].confirm_unverified, false)
  assert.equal(posts[1].confirm_unverified, true)
})
