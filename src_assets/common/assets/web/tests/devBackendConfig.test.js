import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import { DEV_BACKEND, applyDevBackend } from '../devBackendConfig.js'

test('the Web UI gives foundation-ui the backend of the developer-mode GitHub flows once, from its entry', () => {
  assert.equal(DEV_BACKEND.workerUrl, 'https://devapi.alkaidlab.com')
  assert.equal(DEV_BACKEND.clientId, 'Iv23lioKKQ0XBqmts7yF')
  assert.match(DEV_BACKEND.clientId, /^[\w.-]{8,64}$/, 'what devBackend() accepts as configured')
  assert.ok(new URL(DEV_BACKEND.workerUrl).protocol === 'https:' && !new URL(DEV_BACKEND.workerUrl).search)
  let got = null
  applyDevBackend((patch) => { got = patch })
  assert.deepEqual(got, { workerUrl: DEV_BACKEND.workerUrl, clientId: DEV_BACKEND.clientId })
  const entry = fs.readFileSync(new URL('../init.js', import.meta.url), 'utf8')
  assert.match(entry, /import \{ configureDevBackend \} from 'foundation-ui\/vue'/)
  assert.match(entry, /import \{ applyDevBackend \} from '\.\/devBackendConfig\.js'/)
  assert.equal(entry.match(/applyDevBackend\(configureDevBackend\)/g)?.length, 1)
  assert.ok(entry.indexOf('applyDevBackend(configureDevBackend)') < entry.indexOf('export async function initApp'), 'before any page is started')
})
