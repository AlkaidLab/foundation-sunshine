// The audio endpoints of the host (GET /api/audio/devices) as the pickers need them. Pure functions: nothing here
// reads the page or the network, so the rules (what a stored value points at, what the picker offers) are tested alone.

/** The value of the picker entry that opens the field for a name or an id typed by hand. */
export const MANUAL_DEVICE = '__manual__'

const text = (value) => (typeof value === 'string' ? value : '')
const lower = (value) => text(value).toLowerCase()

/** The answer of the host as lists; a missing or unsupported answer is an empty, unsupported one. */
export function normalizeAudioDevices(payload) {
  const empty = { supported: false, render: [], capture: [], defaultOutput: { present: true, reason: '', hostWaiting: false }, vb: { installed: false, state: 'unknown', renderId: '', captureId: '', job: { action: '', step: '', running: false, errorCode: '', detail: '' } } }
  if (!payload || payload.success === false) return empty
  if (payload.supported === false) return { ...empty, vb: { ...empty.vb, state: 'unsupported' } }
  const list = (items) => (Array.isArray(items) ? items : []).map((d) => ({
    id: text(d.id), name: text(d.name), adapter: text(d.adapter), description: text(d.description), state: text(d.state) || 'active',
    formFactor: text(d.form_factor), isDefault: d.default === true, isDefaultCommunications: d.default_communications === true,
    virtual: typeof d.virtual === 'string' ? d.virtual : '',
  })).filter((d) => d.id)
  const vb = payload.vb_cable || {}
  return {
    supported: true,
    render: list(payload.render),
    capture: list(payload.capture),
    // an older host does not say: it counts as present, so no notice is shown on a guess
    defaultOutput: {
      present: payload.default_output?.present !== false,
      reason: text(payload.default_output?.reason),
      hostWaiting: payload.default_output?.host_waiting === true,
    },
    vb: {
      installed: vb.installed === true, state: text(vb.state) || 'missing', renderId: text(vb.render_id), captureId: text(vb.capture_id),
      job: { action: text(vb.job?.action), step: text(vb.job?.step), running: vb.job?.running === true, errorCode: text(vb.job?.error_code), detail: text(vb.job?.detail) },
    },
  }
}

/** Devices a person can pick: the ones that work now. */
export const usableDevices = (list) => list.filter((d) => d.state === 'active')

/**
 * A MMDevice endpoint id such as {0.0.0.00000000}.{29dd7668-45b2-4846-882d-950f55bf7eb8}: what a setting stores for a chosen
 * device. It identifies the device for the host and means nothing to a person, so it is never shown in place of a name.
 */
export const looksLikeEndpointId = (value) => /^\{[0-9a-f.]+\}\.\{[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\}$/i.test(text(value).trim())

/**
 * The device a stored value points at. The host matches the id, the friendly name, the description and the adapter
 * name (src/platform/windows/audio.cpp, match_all_fields), so a value typed by hand or written by an older version still finds its device.
 */
export function findDevice(list, value) {
  const wanted = text(value).trim()
  if (!wanted) return null
  const fields = [(d) => d.id, (d) => d.name, (d) => d.description, (d) => d.adapter]
  for (const field of fields) {
    const hit = list.find((d) => field(d) === wanted)
    if (hit) return hit
  }
  for (const field of fields) {
    const hit = list.find((d) => lower(field(d)) === lower(wanted))
    if (hit) return hit
  }
  return null
}

/**
 * The name a person reads for a stored value: the device it points at, the value itself when someone typed a name the list does not
 * know, and `unknown` for an endpoint id the list does not know (the device is gone): the id itself is never shown ('' for automatic).
 */
export function deviceName(list, value, unknown = 'Selected device') {
  const stored = text(value).trim()
  if (!stored) return ''
  const hit = findDevice(list, stored)
  if (hit) return hit.name
  return looksLikeEndpointId(stored) ? unknown : stored
}

/**
 * The entries of a picker: automatic, every usable device, the devices that cannot be used now with the reason, VB-CABLE when it is
 * not usable, the stored value when no device matches it, and the entry for typing one.
 * `labels`: { auto, manual, missing, unknown, defaultTag, kinds: { vb_cable, ... }, states: { disabled, unplugged }, vb: { missing, disabled, unavailable, partial }, defaultNow(name) }
 * `vb`: the VB-CABLE summary of the host (normalizeAudioDevices().vb), optional.
 * An entry that cannot be chosen now has `disabled: true` and the reason as its text.
 */
export function pickerOptions(list, value, labels, vb = null) {
  const usable = usableDevices(list)
  const current = usable.find((d) => d.isDefault)
  const options = [{ value: '', label: labels.auto, ...(current ? { text: labels.defaultNow(current.name) } : {}) }]
  for (const device of usable) {
    const tags = [labels.kinds[device.virtual] || '', device.isDefault ? labels.defaultTag : ''].filter(Boolean)
    options.push({ value: device.id, label: device.name, ...(tags.length ? { text: tags.join(' · ') } : {}) })
  }
  for (const device of list.filter((d) => d.state !== 'active')) {
    const tags = [labels.kinds[device.virtual] || '', labels.states?.[device.state] || device.state].filter(Boolean)
    options.push({ value: device.id, label: device.name, text: tags.join(' · '), disabled: true })
  }
  // VB-CABLE has no usable playback endpoint: say why instead of leaving it out
  const vbMissing = vb && labels.vb?.[vb.state] && !list.some((d) => d.virtual === 'vb_cable')
  if (vbMissing) options.push({ value: `unavailable:vb_cable`, label: labels.kinds.vb_cable || 'VB-CABLE', text: labels.vb[vb.state], disabled: true })
  const stored = text(value).trim()
  if (stored && !findDevice(list, stored)) options.push({ value: stored, label: looksLikeEndpointId(stored) ? labels.unknown || 'Selected device' : stored, text: labels.missing })
  options.push({ value: MANUAL_DEVICE, label: labels.manual })
  return options
}

/** The value a picker shows for a stored one: the id of the matching device, or the stored text itself. */
export function pickerValue(list, value) {
  const stored = text(value).trim()
  if (!stored) return ''
  return findDevice(usableDevices(list), stored)?.id || findDevice(list, stored)?.id || stored
}

/** What the row shows: the device name, `auto` when nothing is stored, `unknown` for an id nothing matches. */
export function pickerText(list, value, auto, unknown = 'Selected device') {
  const stored = text(value).trim()
  return stored ? deviceName(list, stored, unknown) : auto
}

/**
 * Whether the Steam Streaming Speakers driver is installed, from the playback endpoints it adds: `ready` (an endpoint is active),
 * `disabled` (the endpoint exists but is switched off or unplugged), `missing` (none).
 */
export function steamDriverState(list) {
  const own = list.filter((d) => d.virtual === 'steam_streaming')
  if (own.some((d) => d.state === 'active')) return 'ready'
  return own.length ? 'disabled' : 'missing'
}

/**
 * What a failed install or enable job tells the person, as the key of its text: the download or the pack, the signature,
 * the missing administrator rights, or "the device is still not usable" for everything else.
 */
export function vbCableErrorKind(code) {
  switch (code) {
    case 'VB_DOWNLOAD_FAILED': case 'VB_EXTRACT_FAILED': case 'VB_SETUP_MISSING': return 'download'
    case 'VB_SIGNATURE_INVALID': case 'VB_SIGNER_UNEXPECTED': return 'signature'
    case 'VB_NEEDS_ADMIN': return 'admin'
    case '': return ''
    default: return 'failed'
  }
}

/** The step of a running job as a key suffix (downloading, verifying, installing, enabling); '' when none is running. */
export const vbCableRunningStep = (job) => (job?.running ? (['downloading', 'verifying', 'installing', 'enabling'].includes(job.step) ? job.step : 'downloading') : '')

/**
 * The notice for a computer with no default playback device (a server, a cloud machine, a VM: nothing plugged in or enabled), or null when
 * there is nothing to say. The stream goes on without sound and starts to play by itself when a device appears, so this is a hint, not an error.
 * `reason` is the key suffix of the text: `no_endpoint`, `none_active`, `no_default`, or `unknown` for a reason this page does not know.
 * `ready`: the list was read (a page that is still loading or a failed read says nothing).
 */
export function noDefaultOutputNotice(state, ready = true) {
  if (!ready || !state?.supported || state.defaultOutput?.present !== false) return null
  const known = ['no_endpoint', 'none_active', 'no_default']
  const reason = known.includes(state.defaultOutput.reason) ? state.defaultOutput.reason : 'unknown'
  return { reason, hostWaiting: state.defaultOutput.hostWaiting === true }
}

/** The word of the VB-CABLE state the rows use; `ready` is the only state that needs no action. */
export const vbCableReady = (vb) => vb?.state === 'ready'
