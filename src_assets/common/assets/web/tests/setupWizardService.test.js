import test from 'node:test'
import assert from 'node:assert/strict'

import { saveSetupWizardLocale } from '../services/setupWizardService.js'

test('setup wizard language save preserves the current configuration', async () => {
  const originalFetch = globalThis.fetch
  const requests = []

  globalThis.fetch = async (url, options = {}) => {
    requests.push({ url, options })

    if (requests.length === 1) {
      return new Response(JSON.stringify({
        locale: 'en',
        output_name: 'ZakoHDR',
        port: 40989,
        setup_wizard_completed: false,
      }), { status: 200 })
    }

    return new Response(JSON.stringify({ status: true }), { status: 200 })
  }

  try {
    const response = await saveSetupWizardLocale('zh')

    assert.equal(response.ok, true)
    assert.equal(requests.length, 2)
    assert.equal(requests[0].url, '/api/config')
    assert.equal(requests[0].options.method, undefined)
    assert.equal(requests[1].url, '/api/config')
    assert.equal(requests[1].options.method, 'POST')
    assert.equal(requests[1].options.headers.get('Content-Type'), 'application/json')
    assert.deepEqual(JSON.parse(requests[1].options.body), {
      locale: 'zh',
      output_name: 'ZakoHDR',
      port: 40989,
      setup_wizard_completed: false,
    })
  } finally {
    globalThis.fetch = originalFetch
  }
})
