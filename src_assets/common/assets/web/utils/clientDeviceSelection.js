// The website groups downloads by playing device, not by product.
export const CLIENT_DEVICES = ['android', 'windows', 'harmony', 'ios', 'macos', 'linux', 'steam', 'android-tv', 'quest', 'apple-tv']

export function clientsForDevice(clients, device) {
  const friendPlatform = { android: 'crown', harmony: 'baozi' }[device]
  const matches = clients.flatMap((client) => client.targets
    .filter((target) => target.platform === device || target.platform === friendPlatform)
    .map((target) => ({ client, target })))

  if (device === 'macos') {
    const enhanced = matches.findIndex(({ client }) => client.id === 'macos-enhanced')
    if (enhanced > 0) matches.unshift(...matches.splice(enhanced, 1))
  }
  return matches
}
