const JSON_CONTENT_TYPE = 'application/json'

const isPlainBody = (body) =>
  body &&
  typeof body === 'object' &&
  !(body instanceof FormData) &&
  !(body instanceof Blob) &&
  !(body instanceof ArrayBuffer) &&
  !(body instanceof URLSearchParams)

const buildOptions = (options = {}) => {
  const next = { ...options }
  const headers = new Headers(options.headers || {})

  if (isPlainBody(next.body)) {
    if (!headers.has('Content-Type')) {
      headers.set('Content-Type', JSON_CONTENT_TYPE)
    }
    next.body = JSON.stringify(next.body)
  }

  next.headers = headers
  return next
}

export const parseJsonResponse = async (response, fallback = {}) => response.json().catch(() => fallback)

export const getApiErrorMessage = (data, response) => {
  const error = data?.error
  if (typeof error === 'string') return error
  if (typeof error?.message === 'string') return error.message
  if (typeof data?.message === 'string') return data.message
  return `HTTP ${response.status}`
}

export async function apiFetch(url, options = {}) {
  return fetch(url, buildOptions(options))
}

export async function apiJson(url, options = {}) {
  const response = await apiFetch(url, options)
  const data = await parseJsonResponse(response)

  if (!response.ok) {
    throw new Error(getApiErrorMessage(data, response))
  }

  return data
}

export const apiPostJson = (url, body = {}, options = {}) =>
  apiJson(url, {
    ...options,
    method: options.method || 'POST',
    body,
  })
