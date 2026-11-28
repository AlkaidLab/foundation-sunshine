import test from 'node:test'
import assert from 'node:assert/strict'
import { AppService } from '../services/appService.js'

test('application save rejects a business error even when HTTP succeeds', async (context) => {
  context.mock.method(console, 'error', () => {})
  context.mock.method(globalThis, 'fetch', async () => new Response(JSON.stringify({ status: 'false', error: 'Invalid Input JSON' }), { status: 200 }))
  await assert.rejects(AppService.saveApps([], null), /Invalid Input JSON/)
})
