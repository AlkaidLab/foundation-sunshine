import test from 'node:test'
import assert from 'node:assert/strict'
import { checkHttpPort, hostPorts } from '../utils/portCheck.js'

test('the ports of a base port are the ones the host opens, and only the plain HTTP one can be tried from a browser', () => {
  const ports = hostPorts(47989)
  assert.deepEqual(ports.map((p) => [p.id, p.protocol, p.port]), [['https', 'TCP', '47984'], ['http', 'TCP', '47989'], ['web', 'TCP', '47990'], ['rtsp', 'TCP', '48010'], ['stream', 'UDP', '47998 – 48001']])
  assert.deepEqual(ports.map((p) => p.check), [false, true, 'self', false, false])
  assert.equal(hostPorts('58000')[1].port, '58000')
})

test('a request that answers is reachable; an error or a timeout is not', async () => {
  const seen = []
  const ok = async (url, init) => { seen.push([url, init.mode]); return {} }
  assert.equal(await checkHttpPort('192.0.2.5', 47989, { fetchImpl: ok }), 'reachable')
  assert.deepEqual(seen, [['http://192.0.2.5:47989/serverinfo', 'no-cors']])
  assert.equal(await checkHttpPort('h', 1, { fetchImpl: async () => { throw new TypeError('Failed to fetch') } }), 'unreachable')
  const slow = (url, init) => new Promise((resolve, reject) => init.signal.addEventListener('abort', () => reject(new Error('aborted'))))
  assert.equal(await checkHttpPort('h', 1, { fetchImpl: slow, timeoutMs: 20 }), 'unreachable')
})
