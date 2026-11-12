// Reachability of a port from the device that shows the page. A browser can only try an HTTP request, so only the plain
// HTTP port of the host can be tried (the HTTPS ones fail on the certificate and UDP cannot be tried at all). An opaque
// answer (no-cors) is a connection that worked; an error or a timeout is one that did not.
export async function checkHttpPort(host, port, { fetchImpl = globalThis.fetch, timeoutMs = 3000, path = '/serverinfo' } = {}) {
  const controller = new AbortController()
  const timer = setTimeout(() => controller.abort(), timeoutMs)
  try {
    await fetchImpl(`http://${host}:${port}${path}`, { mode: 'no-cors', cache: 'no-store', signal: controller.signal })
    return 'reachable'
  } catch {
    return 'unreachable'
  } finally {
    clearTimeout(timer)
  }
}

/** The ports a base port stands for: [{ id, protocol, port, note, check }]; `check` says whether a browser can try it. */
export function hostPorts(base) {
  const b = Number(base)
  return [
    { id: 'https', protocol: 'TCP', port: String(b - 5), note: 'HTTPS API', check: false },
    { id: 'http', protocol: 'TCP', port: String(b), note: 'HTTP API', check: true },
    { id: 'web', protocol: 'TCP', port: String(b + 1), note: 'web', check: 'self' },
    { id: 'rtsp', protocol: 'TCP', port: String(b + 21), note: 'RTSP', check: false },
    { id: 'stream', protocol: 'UDP', port: `${b + 9} – ${b + 12}`, note: 'stream', check: false },
  ]
}
