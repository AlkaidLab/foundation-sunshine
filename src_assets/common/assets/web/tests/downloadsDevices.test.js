import './helpers/foundationUi.mjs'
import test from 'node:test'
import assert from 'node:assert/strict'
import { CLIENT_DEVICES, clientsForDevice, deviceForPlatform } from '../utils/clientDeviceSelection.js'
import { CLIENT_PLATFORMS } from '../config/clientDownloads.js'

const { CLIENTS } = await import('foundation-ui/clients')

test('partner platform deep links select the device containing the requested client', () => {
  for (const [platform, device] of [['crown', 'android'], ['baozi', 'harmony']]) {
    assert.equal(deviceForPlatform(platform), device)
    assert.ok(clientsForDevice(CLIENTS, deviceForPlatform(platform)).some(({ target }) => target.platform === platform))
  }
  assert.equal(deviceForPlatform('windows'), 'windows')
  assert.equal(deviceForPlatform('unknown'), undefined)
})

test('each playing device has its own client selection', () => {
  assert.deepEqual(CLIENT_DEVICES, ['android', 'windows', 'harmony', 'ios', 'macos', 'linux', 'steam', 'android-tv', 'quest', 'apple-tv'])
  for (const device of CLIENT_DEVICES) assert.ok(clientsForDevice(CLIENTS, device).length, device)
  assert.deepEqual(clientsForDevice(CLIENTS, 'android').map(({ client }) => client.id), ['vplus-android', 'crown'])
  assert.deepEqual(clientsForDevice(CLIENTS, 'quest').map(({ client }) => client.id), ['vplus-android'])
  assert.deepEqual(clientsForDevice(CLIENTS, 'macos').map(({ client }) => client.id), ['macos-enhanced', 'vplus-desktop'])
  assert.deepEqual(clientsForDevice(CLIENTS, 'apple-tv').map(({ client }) => client.id), ['voidlink'])
  const community = CLIENTS.filter(({ group }) => group === 'partner')
  assert.deepEqual(community.filter(({ recommended }) => recommended).map(({ id }) => id), ['voidlink'])
})

test('VoidLink uses the same App Store links for iOS and Apple TV', () => {
  const ios = CLIENT_PLATFORMS.find(({ id }) => id === 'ios')
  const tv = CLIENT_PLATFORMS.find(({ id }) => id === 'apple-tv')
  assert.deepEqual(tv.links, ios.links)
  assert.deepEqual(ios.links.map(({ id }) => id), ['store-cn', 'store-global'])
  for (const locale of ['zh', 'en']) assert.equal(tv.getDownloadUrls(locale).primary, ios.getDownloadUrls(locale).primary)
  assert.equal(ios.links.find(({ id }) => id === 'store-cn').url, 'https://apps.apple.com/cn/app/voidlink/id6747717070')
  assert.equal(ios.links.find(({ id }) => id === 'store-global').url, 'https://apps.apple.com/us/app/voidlink-extreme/id6755103808')
  assert.ok(tv.links.every(({ url }) => !url.includes('testflight.apple.com')))
})
