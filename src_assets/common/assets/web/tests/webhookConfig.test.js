import test from 'node:test'
import assert from 'node:assert/strict'

import {
  loadWebhookConfig,
  saveWebhookConfig,
  sendWebhookTest,
} from '../services/webhookService.js'
import {
  buildWebhookConfigRequest,
  parseWebhookEventIds,
  serializeWebhookEventIds,
  buildWebhookTestRequest,
  normalizeWebhookConfigResponse,
  normalizeWebhookTestRetries,
  webhookTimeoutToMilliseconds,
  webhookTimeoutToSeconds,
} from '../utils/webhookConfig.js'

const withMockFetch = async (handler, run) => {
  const originalFetch = globalThis.fetch
  globalThis.fetch = handler
  try {
    await run()
  } finally {
    globalThis.fetch = originalFetch
  }
}

test('Webhook timeout rounds millisecond configuration up to whole seconds', () => {
  assert.equal(webhookTimeoutToSeconds(1000), 1)
  assert.equal(webhookTimeoutToSeconds(1001), 2)
  assert.equal(webhookTimeoutToSeconds(15000), 15)
})

test('Webhook timeout stores the seconds input as bounded milliseconds', () => {
  assert.equal(webhookTimeoutToMilliseconds(1), 1000)
  assert.equal(webhookTimeoutToMilliseconds(3), 3000)
  assert.equal(webhookTimeoutToMilliseconds(8), 8000)
  assert.equal(webhookTimeoutToMilliseconds(16), 15000)
})

test('Webhook test retries default to zero and stay within zero to three', () => {
  assert.equal(normalizeWebhookTestRetries(undefined), 0)
  assert.equal(normalizeWebhookTestRetries(-1), 0)
  assert.equal(normalizeWebhookTestRetries(2.9), 2)
  assert.equal(normalizeWebhookTestRetries(4), 3)
})

test('Webhook event configuration uses a stable sorted comma-separated ID list', () => {
  assert.deepEqual(parseWebhookEventIds(undefined), [0, 1, 2, 3, 4, 5, 6])
  assert.deepEqual(parseWebhookEventIds('6,2,2,invalid,9,0'), [0, 2, 6])
  assert.deepEqual(parseWebhookEventIds('invalid,9'), [])
  assert.deepEqual(parseWebhookEventIds('-1'), [])
  assert.equal(serializeWebhookEventIds([6, 2, 2, 9, 0]), '0,2,6')
  assert.equal(serializeWebhookEventIds([]), '-1')
})

test('Webhook test request keeps backend millisecond and TLS field types', () => {
  assert.deepEqual(buildWebhookTestRequest({
    webhook_url: 'https://example.invalid/webhook',
    webhook_skip_ssl_verify: 'disabled',
    webhook_timeout: 2000,
    webhook_retries: 2,
  }), {
    webhook_url: 'https://example.invalid/webhook',
    webhook_skip_ssl_verify: false,
    webhook_timeout: 2000,
    webhook_retries: 2,
  })
})

test('Standalone Webhook config converts native JSON values to UI values', () => {
  assert.deepEqual(normalizeWebhookConfigResponse({
    webhook_enabled: true,
    webhook_url: 'https://example.invalid/webhook',
    webhook_skip_ssl_verify: false,
    webhook_timeout: 1001,
    webhook_events: '6,2,0',
  }), {
    webhook_enabled: 'enabled',
    webhook_url: 'https://example.invalid/webhook',
    webhook_skip_ssl_verify: 'disabled',
    webhook_timeout: 2000,
    webhook_events: '0,2,6',
  })
})

test('Standalone Webhook save request has the complete native JSON schema', () => {
  assert.deepEqual(buildWebhookConfigRequest({
    webhook_enabled: 'enabled',
    webhook_url: 'https://example.invalid/webhook',
    webhook_skip_ssl_verify: 'disabled',
    webhook_timeout: 15000,
    webhook_events: '6,0,2',
  }), {
    webhook_enabled: true,
    webhook_url: 'https://example.invalid/webhook',
    webhook_skip_ssl_verify: false,
    webhook_timeout: 15000,
    webhook_events: '0,2,6',
  })
})

test('Webhook config uses its standalone authenticated endpoint', async () => {
  const requests = []
  await withMockFetch(
    async (url, options = {}) => {
      requests.push({ url, options })
      if ((options.method || 'GET') === 'GET') {
        return new Response(JSON.stringify({
          status: true,
          webhook_enabled: false,
          webhook_url: '',
          webhook_skip_ssl_verify: false,
          webhook_timeout: 5000,
          webhook_events: '0,1,2,3,4,5,6',
        }), { status: 200 })
      }
      return new Response(JSON.stringify({ status: true }), { status: 200 })
    },
    async () => {
      assert.deepEqual(await loadWebhookConfig(), {
        webhook_enabled: 'disabled',
        webhook_url: '',
        webhook_skip_ssl_verify: 'disabled',
        webhook_timeout: 5000,
        webhook_events: '0,1,2,3,4,5,6',
      })
      await saveWebhookConfig({
        webhook_enabled: 'enabled',
        webhook_url: 'https://example.invalid/webhook',
        webhook_skip_ssl_verify: 'enabled',
        webhook_timeout: 3000,
        webhook_events: '1,4',
      })
    }
  )

  assert.equal(requests[0].url, '/api/webhook/config')
  assert.equal(requests[0].options.method, undefined)
  assert.equal(requests[1].url, '/api/webhook/config')
  assert.equal(requests[1].options.method, 'POST')
  assert.deepEqual(JSON.parse(requests[1].options.body), {
    webhook_enabled: true,
    webhook_url: 'https://example.invalid/webhook',
    webhook_skip_ssl_verify: true,
    webhook_timeout: 3000,
    webhook_events: '1,4',
  })
})

test('Webhook test uses the authenticated same-origin API endpoint', async () => {
  await withMockFetch(
    async (url, options) => {
      assert.equal(url, '/api/webhook/test')
      assert.equal(options.method, 'POST')
      assert.deepEqual(JSON.parse(options.body), {
        webhook_url: 'https://example.invalid/webhook',
        webhook_skip_ssl_verify: true,
        webhook_timeout: 3000,
        webhook_retries: 0,
      })
      return new Response(JSON.stringify({ status: true }), { status: 200 })
    },
    async () => {
      const result = await sendWebhookTest({
        webhook_url: 'https://example.invalid/webhook',
        webhook_skip_ssl_verify: 'enabled',
        webhook_timeout: 3000,
      })
      assert.deepEqual(result, { status: true })
    }
  )
})

test('Webhook config read preserves the stable damaged-file error code', async () => {
  await withMockFetch(
    async () => new Response(JSON.stringify({
      status: false,
      error: 'Failed to read Webhook configuration',
      error_code: 'webhook_config_invalid',
    }), { status: 500 }),
    async () => {
      await assert.rejects(
        loadWebhookConfig(),
        (error) => error.code === 'webhook_config_invalid' && error.status === 500
      )
    }
  )
})
