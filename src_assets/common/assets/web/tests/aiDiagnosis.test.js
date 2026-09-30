import test from 'node:test'
import assert from 'node:assert/strict'
import { useAiDiagnosis } from '../composables/useAiDiagnosis.js'
import { API_ENDPOINTS } from '../utils/constants.js'

const remoteConfig = {
  enabled: true,
  provider: 'openai',
  apiBase: 'https://api.openai.com/v1',
  model: 'gpt-4.1-mini',
  apiKeyConfigured: true,
}

function mockAiFetch(t, getConfig, requests) {
  t.mock.method(globalThis, 'fetch', async (url, options = {}) => {
    requests.push({ url, options })
    if (url === API_ENDPOINTS.AI_CONFIG) {
      return Response.json(getConfig())
    }
    assert.equal(url, API_ENDPOINTS.AI_CHAT_COMPLETIONS)
    return Response.json({ choices: [{ message: { content: 'Diagnosis completed.' } }] })
  })
}

test('diagnosis uses a server-stored key without receiving or sending the secret', async (t) => {
  const requests = []
  mockAiFetch(t, () => remoteConfig, requests)
  const diagnosis = useAiDiagnosis()

  await diagnosis.diagnose('Error: Encoder initialization failed')

  assert.equal(diagnosis.error.value, '')
  assert.equal(diagnosis.result.value, 'Diagnosis completed.')
  assert.equal(diagnosis.config.apiKey, '')
  const chat = requests.find(({ url }) => url === API_ENDPOINTS.AI_CHAT_COMPLETIONS)
  assert.ok(chat)
  assert.equal(chat.options.headers.Authorization, undefined)
  const body = JSON.parse(chat.options.body)
  assert.equal(Object.hasOwn(body, 'apiKey'), false)
  assert.equal(body.model, remoteConfig.model)
  assert.ok(diagnosis.localFindings.value.length > 0)
})

test('diagnosis reloads key status and blocks cloud requests after the key is cleared', async (t) => {
  let config = remoteConfig
  const requests = []
  mockAiFetch(t, () => config, requests)
  const diagnosis = useAiDiagnosis()
  await diagnosis.diagnose('Info: Streaming started')
  requests.length = 0
  config = { ...remoteConfig, apiKeyConfigured: false }

  await diagnosis.diagnose('Error: Encoder initialization failed')

  assert.equal(diagnosis.error.value, 'Please configure an API key first.')
  assert.deepEqual(requests.map(({ url }) => url), [API_ENDPOINTS.AI_CONFIG])
  assert.ok(diagnosis.localFindings.value.length > 0)
})

test('diagnosis allows a local Ollama endpoint without an API key', async (t) => {
  const requests = []
  mockAiFetch(t, () => ({
    ...remoteConfig,
    provider: 'ollama',
    apiBase: 'http://localhost:11434/v1',
    apiKeyConfigured: false,
  }), requests)

  const diagnosis = useAiDiagnosis()
  await diagnosis.diagnose('Info: Streaming started')

  assert.equal(diagnosis.error.value, '')
  assert.equal(diagnosis.result.value, 'Diagnosis completed.')
})
