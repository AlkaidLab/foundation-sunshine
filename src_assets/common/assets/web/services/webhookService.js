import { apiJson, apiPostJson } from '../utils/apiFetch.js'
import {
  buildWebhookConfigRequest,
  buildWebhookTestRequest,
  normalizeWebhookConfigResponse,
} from '../utils/webhookConfig.js'

const WEBHOOK_CONFIG_ENDPOINT = '/api/webhook/config'
const WEBHOOK_TEST_ENDPOINT = '/api/webhook/test'

export const loadWebhookConfig = async () =>
  normalizeWebhookConfigResponse(await apiJson(WEBHOOK_CONFIG_ENDPOINT))

export const saveWebhookConfig = (config) =>
  apiPostJson(WEBHOOK_CONFIG_ENDPOINT, buildWebhookConfigRequest(config))

export const sendWebhookTest = (config) =>
  apiPostJson(WEBHOOK_TEST_ENDPOINT, buildWebhookTestRequest(config))
