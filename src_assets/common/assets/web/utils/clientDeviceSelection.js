// The website groups downloads by playing device, not by product.
export const CLIENT_DEVICES = ['android', 'windows', 'harmony', 'ios', 'macos', 'linux', 'steam', 'android-tv', 'quest', 'apple-tv']

const PARTNER_PLATFORMS = { android: 'crown', harmony: 'baozi' }

export function deviceForPlatform(platform) {
  if (CLIENT_DEVICES.includes(platform)) return platform
  return Object.keys(PARTNER_PLATFORMS).find((device) => PARTNER_PLATFORMS[device] === platform)
}

export function clientsForDevice(clients, device) {
  const friendPlatform = PARTNER_PLATFORMS[device]
  const matches = clients.flatMap((client) => client.targets
    .filter((target) => target.platform === device || target.platform === friendPlatform)
    .map((target) => ({ client, target })))

  if (device === 'macos') {
    const enhanced = matches.findIndex(({ client }) => client.id === 'macos-enhanced')
    if (enhanced > 0) matches.unshift(...matches.splice(enhanced, 1))
  }
  return matches
}
