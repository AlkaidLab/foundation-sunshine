// The audio endpoints of the host (GET /api/audio/devices) as the pickers need them. Pure functions: nothing here
// reads the page or the network, so the rules (what a stored value points at, what the picker offers) are tested alone.

/** The value of the picker entry that opens the field for a name or an id typed by hand. */
export const MANUAL_DEVICE = '__manual__'

const text = (value) => (typeof value === 'string' ? value : '')
const lower = (value) => text(value).toLowerCase()

/** The answer of the host as lists; a missing or unsupported answer is an empty, unsupported one. */
export function normalizeAudioDevices(payload) {
  const empty = { supported: false, render: [], capture: [], vb: { installed: false, state: 'unknown', renderId: '', captureId: '', job: { action: '', step: '', running: false, errorCode: '', detail: '' } } }
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
    vb: {
      installed: vb.installed === true, state: text(vb.state) || 'missing', renderId: text(vb.render_id), captureId: text(vb.capture_id),
      job: { action: text(vb.job?.action), step: text(vb.job?.step), running: vb.job?.running === true, errorCode: text(vb.job?.error_code), detail: text(vb.job?.detail) },
    },
  }
}

/** Devices a person can pick: the ones that work now. */
export const usableDevices = (list) => list.filter((d) => d.state === 'active')

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

/** The name of the device a stored value points at, or the value itself when the list does not know it ('' for automatic). */
export const deviceName = (list, value) => findDevice(list, value)?.name || text(value)

/**
 * The entries of a picker: automatic, every usable device, the stored value when no device matches it, and the entry for typing one.
 * `labels`: { auto, manual, missing, defaultTag, kinds: { vb_cable, ... }, defaultNow(name) }
 */
export function pickerOptions(list, value, labels) {
  const usable = usableDevices(list)
  const current = usable.find((d) => d.isDefault)
  const options = [{ value: '', label: labels.auto, ...(current ? { text: labels.defaultNow(current.name) } : {}) }]
  for (const device of usable) {
    const tags = [labels.kinds[device.virtual] || '', device.isDefault ? labels.defaultTag : ''].filter(Boolean)
    options.push({ value: device.id, label: device.name, ...(tags.length ? { text: tags.join(' · ') } : {}) })
  }
  const stored = text(value).trim()
  if (stored) {
    const hit = findDevice(usable, stored)
    if (!hit) options.push({ value: stored, label: stored, text: labels.missing })
  }
  options.push({ value: MANUAL_DEVICE, label: labels.manual })
  return options
}

/** The value a picker shows for a stored one: the id of the matching device, or the stored text itself. */
export function pickerValue(list, value) {
  const stored = text(value).trim()
  if (!stored) return ''
  return findDevice(usableDevices(list), stored)?.id || stored
}

/** What the row shows: the device name, or `auto` when nothing is stored. */
export function pickerText(list, value, auto) {
  const stored = text(value).trim()
  return stored ? deviceName(list, stored) : auto
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

/** The word of the VB-CABLE state the rows use; `ready` is the only state that needs no action. */
export const vbCableReady = (vb) => vb?.state === 'ready'
