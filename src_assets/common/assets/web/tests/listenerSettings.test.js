import test from 'node:test'
import assert from 'node:assert/strict'
import { isIpAddress, validateListenerSettings } from '../utils/listenerSettings.js'
import { useConfig } from '../composables/useConfig.js'

test('listening addresses allow single IPv4 and scoped IPv6 but reject lists and CIDR', () => {
  for (const address of ['192.168.1.10', '127.0.0.1', '::', 'fd00::10', 'fe80::1%12']) assert.equal(isIpAddress(address), true, address)
  for (const address of ['192.168.1.0/24', '192.168.1.10,10.0.0.1', 'localhost', '999.0.0.1', '1.2.03.4', '::%']) assert.equal(isIpAddress(address), false, address)
})
test('listening configuration checks family compatibility and all service port offsets', () => {
  assert.equal(validateListenerSettings({}), '')
  assert.equal(validateListenerSettings({ bind_address: '::1', address_family: 'ipv4' }), 'connections.ipv6_requires_both')
  assert.equal(validateListenerSettings({ bind_address: '::1', address_family: 'both' }), '')
  assert.equal(validateListenerSettings({ bind_address: '127.0.0.1', address_family: 'both' }), '')
  for (const port of [1028, 65515, 47989.5, 'text']) assert.equal(validateListenerSettings({ port }), 'connections.invalid_port')
  for (const port of [1029, 47989, 65514]) assert.equal(validateListenerSettings({ port }), '')
})

test('the client-page link opens Network and reveals the bind address setting', async () => {
  const originalWindow = globalThis.window
  const originalDocument = globalThis.document
  let revealed = false
  globalThis.window = { location: { hash: '#bind_address' } }
  globalThis.document = { getElementById: (id) => id === 'bind_address' ? { scrollIntoView: () => { revealed = true } } : null }
  try {
    const config = useConfig()
    config.initTabs()
    config.handleHash()
    assert.equal(config.currentTab.value, 'network')
    await new Promise((resolve) => setTimeout(resolve, 120))
    assert.equal(revealed, true)
  } finally { globalThis.window = originalWindow; globalThis.document = originalDocument }
})
