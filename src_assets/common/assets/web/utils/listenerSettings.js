export const isIpAddress = (value) => {
  if (typeof value !== 'string' || !value || value.includes('/')) return false
  if (value.includes(':')) {
    const [address, scope, ...extra] = value.split('%')
    if (extra.length || (scope !== undefined && !/^[\w.-]+$/.test(scope))) return false
    try { return new URL(`http://[${address}]/`).hostname.startsWith('[') } catch { return false }
  }
  const parts = value.split('.')
  return parts.length === 4 && parts.every((part) => /^(0|[1-9]\d{0,2})$/.test(part) && Number(part) <= 255)
}

export const validateListenerSettings = (config) => {
  const bind = String(config.bind_address ?? '').trim()
  if (bind && !isIpAddress(bind)) return 'connections.invalid_bind'
  if (bind.includes(':') && (config.address_family || 'ipv4') !== 'both') return 'connections.ipv6_requires_both'
  if (config.address_family && !['ipv4', 'both'].includes(config.address_family)) return 'connections.invalid_family'
  const port = config.port === '' || config.port == null ? 47989 : Number(config.port)
  if (!Number.isInteger(port) || port < 1029 || port > 65514) return 'connections.invalid_port'
  return ''
}
