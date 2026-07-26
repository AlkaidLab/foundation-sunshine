export const WEBHOOK_EVENT_IDS = Object.freeze([0, 1, 2, 3, 4, 5, 6])

export const parseWebhookEventIds = (value) => {
  if (value === undefined || value === null) {
    return [...WEBHOOK_EVENT_IDS]
  }

  if (String(value).trim() === '-1') {
    return []
  }

  const selected = new Set()
  for (const token of String(value).split(',')) {
    const trimmed = token.trim()
    if (!/^\d+$/.test(trimmed)) {
      continue
    }
    const id = Number(trimmed)
    if (WEBHOOK_EVENT_IDS.includes(id)) {
      selected.add(id)
    }
  }
  return [...selected].sort((left, right) => left - right)
}

export const serializeWebhookEventIds = (eventIds) => {
  const selected = new Set(
    eventIds
      .map(Number)
      .filter((id) => WEBHOOK_EVENT_IDS.includes(id))
  )
  const serialized = [...selected].sort((left, right) => left - right).join(',')
  return serialized || '-1'
}

export const webhookTimeoutToSeconds = (milliseconds) => {
  const value = Number(milliseconds) || 5000
  return Math.min(15, Math.max(1, Math.ceil(value / 1000)))
}

export const webhookTimeoutToMilliseconds = (seconds) => {
  const value = Math.ceil(Number(seconds) || 1)
  return Math.min(15, Math.max(1, value)) * 1000
}

export const normalizeWebhookTestRetries = (retryCount) => {
  const value = Math.trunc(Number(retryCount) || 0)
  return Math.min(3, Math.max(0, value))
}

const isEnabled = (value) =>
  value === true ||
  value === 1 ||
  String(value).trim().toLowerCase() === 'enabled' ||
  String(value).trim().toLowerCase() === 'true' ||
  String(value).trim() === '1'

const normalizeTimeoutMilliseconds = (milliseconds) =>
  webhookTimeoutToMilliseconds(webhookTimeoutToSeconds(milliseconds))

export const normalizeWebhookConfigResponse = (config = {}) => ({
  webhook_enabled: isEnabled(config.webhook_enabled) ? 'enabled' : 'disabled',
  webhook_url: typeof config.webhook_url === 'string' ? config.webhook_url : '',
  webhook_skip_ssl_verify: isEnabled(config.webhook_skip_ssl_verify) ? 'enabled' : 'disabled',
  webhook_timeout: normalizeTimeoutMilliseconds(config.webhook_timeout),
  webhook_events: serializeWebhookEventIds(parseWebhookEventIds(config.webhook_events)),
})

export const buildWebhookConfigRequest = (config) => ({
  webhook_enabled: isEnabled(config.webhook_enabled),
  webhook_url: typeof config.webhook_url === 'string' ? config.webhook_url : '',
  webhook_skip_ssl_verify: isEnabled(config.webhook_skip_ssl_verify),
  webhook_timeout: normalizeTimeoutMilliseconds(config.webhook_timeout),
  webhook_events: serializeWebhookEventIds(parseWebhookEventIds(config.webhook_events)),
})

export const buildWebhookTestRequest = (config) => ({
  webhook_url: typeof config.webhook_url === 'string' ? config.webhook_url : '',
  webhook_skip_ssl_verify: isEnabled(config.webhook_skip_ssl_verify),
  webhook_timeout: normalizeTimeoutMilliseconds(config.webhook_timeout),
  webhook_retries: normalizeWebhookTestRetries(config.webhook_retries),
})
