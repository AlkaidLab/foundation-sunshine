import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { SEARCH_INDEX } from 'foundation-ui/settings/index.js'
import { CLIENT_KEYS, HOT_KEYS, needsRestart, setRestartPlatform } from '../composables/restartKeys.js'
import { keysOf } from '../composables/settingsBinding.js'

// the list is checked against the host source it was derived from, so it cannot drift from it
const host = readFileSync(new URL('../../../../../src/config.cpp', import.meta.url), 'utf8')
const body = host.slice(host.search(/void\s+apply_config\(std::unordered_map/), host.indexOf('parse(int argc, char *argv[])'))
const read = new Set([...body.matchAll(/"([a-z0-9_]+)"/g)].map((m) => m[1]))
for (const m of host.matchAll(/_f\(\s*vars,\s*"([a-z0-9_]+)"/g)) read.add(m[1])

// the keys the controls of the page write (a control such as the codec strategy writes two modes, not a key of its own name)
const keys = [...new Set(SEARCH_INDEX.flatMap((e) => keysOf(e.id)))]

test('a key of the page that apply_config does not read is hot: the host has nothing to restart for it', () => {
  const unread = keys.filter((key) => !read.has(key))
  assert.deepEqual(unread, [], 'the host reads every key of the page now (external_endpoints and stun_servers came in with the network overview): add the key here when one is not read')
  for (const key of unread) assert.equal(needsRestart(key), false, `${key} is not read by apply_config but is offered a restart`)
})

test('the codec strategy and its HDR switch write hevc_mode and av1_mode, which the host reads at start: they wait for the restart', () => {
  assert.deepEqual(keysOf('codec_strategy'), ['hevc_mode', 'av1_mode'])
  assert.equal(needsRestart('hevc_mode') && needsRestart('av1_mode'), true)
})

test('every other key waits for the restart, except the ones the host applies at once, at the next launch, or never reads again', () => {
  setRestartPlatform('')
  const readByHost = keys.filter((key) => read.has(key))
  const hotAndRead = readByHost.filter((id) => !needsRestart(id)).sort()
  assert.deepEqual(hotAndRead, ['launch_splash', 'notify_pre_releases', 'nvenc_manage_driver_settings', 'nvenc_vdd_force_vsync'].filter((k) => readByHost.includes(k)))
  assert.ok(needsRestart('port') && needsRestart('sunshine_name') && needsRestart('upnp') && needsRestart('resolutions'))
  // config::nvhttp.external_endpoints / stun_servers are the startup copy that clients.cpp and confighttp/network.cpp read; a save does not refresh them
  assert.ok(needsRestart('external_endpoints') && needsRestart('stun_servers'))
})

test('gamepad applies at once only where the host implements the setter: the next controller on Windows, a restart on Linux and macOS', () => {
  for (const platform of ['linux', 'macos', '', 'unknown']) {
    setRestartPlatform(platform)
    assert.equal(needsRestart('gamepad'), true, `${platform || 'unknown'}: the setter is empty, the save waits for the restart`)
  }
  setRestartPlatform('windows')
  assert.equal(needsRestart('gamepad'), false)
  assert.ok(HOT_KEYS.includes('gamepad'))
  assert.match(host, /updates\.find\("gamepad"\)[\s\S]{0,200}set_global_gamepad_mode/)
  setRestartPlatform('')
})

test('notify_pre_releases is parsed at start but nothing in the running process reads it: the page reads the saved file', () => {
  assert.equal(needsRestart('notify_pre_releases'), false)
})

test('the keys the client decides at launch still wait for the restart: the host copy is the cap, the list or the advertisement', () => {
  for (const key of CLIENT_KEYS) assert.equal(needsRestart(key), true, `${key}: the host copy is read from the startup memory`)
})
