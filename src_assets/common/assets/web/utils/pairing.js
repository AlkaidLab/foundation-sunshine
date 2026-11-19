// Small pure rules of the clients page.

export const PIN_LENGTH = 4

/** The pairing form can be sent: a full PIN and a device name. */
export function canSubmitPairing(pin, name, length = PIN_LENGTH) {
  return /^[0-9]+$/.test(pin) && pin.length === length && String(name ?? '').trim().length > 0
}

/** State of a paired client for its row: 'streaming' | 'disabled' | 'allowed'. */
export function clientState(client) {
  if (client?.enabled === false) return 'disabled'
  return client?.streaming ? 'streaming' : 'allowed'
}

/** Keys of the brightness fields whose value is outside the allowed range (manual mode only). */
export function brightnessErrors(client, fields) {
  if (!client || client.hdrBrightnessMode !== 'manual') return []
  return fields.filter((f) => {
    const v = Number(client[f.key])
    return !Number.isFinite(v) || v < f.min || v > f.max
  }).map((f) => f.key)
}

/** Whether the device settings can be saved. */
export function canSaveDevice(client, fields) {
  return !!client && String(client.name ?? '').trim().length > 0 && brightnessErrors(client, fields).length === 0
}
