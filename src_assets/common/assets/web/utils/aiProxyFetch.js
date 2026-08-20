export function isSunshineUnavailableError(error) {
  return String(error?.message || error || '').includes('Sunshine service is unavailable')
}

export async function refreshSunshineProxyTarget() {
  try {
    const invoke = globalThis.window?.__TAURI__?.core?.invoke
    if (!invoke) return false
    await invoke('refresh_sunshine_target')
    return true
  } catch {
    return false
  }
}

export async function fetchAiJson(url, options = {}) {
  async function run() {
    const response = await fetch(url, options)
    const data = await response.json().catch(() => ({}))
    if (!response.ok || data.status === 'error') {
      const message = typeof data.error === 'string' ? data.error : data.error?.message
      throw new Error(message || `Request failed: ${response.status}`)
    }
    return data
  }

  try {
    return await run()
  } catch (error) {
    if (!isSunshineUnavailableError(error) || !(await refreshSunshineProxyTarget())) {
      throw error
    }
    return run()
  }
}
