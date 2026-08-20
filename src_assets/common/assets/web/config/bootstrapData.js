import { apiJson } from '../utils/apiFetch.js'

let configRequest = null
let localeRequest = null

const cachedRequest = (path, getCurrent, setCurrent) => {
  const current = getCurrent()
  if (current) return current

  const request = apiJson(path)
  setCurrent(request)
  request.catch(() => {
    if (getCurrent() === request) setCurrent(null)
  })
  return request
}

export const getBootstrapConfig = () =>
  cachedRequest('/api/config', () => configRequest, (request) => { configRequest = request })

export const getBootstrapLocale = () =>
  cachedRequest('/api/configLocale', () => localeRequest, (request) => { localeRequest = request })

export const resetBootstrapDataCache = () => {
  configRequest = null
  localeRequest = null
}
