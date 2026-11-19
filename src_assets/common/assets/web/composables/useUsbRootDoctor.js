import { apiFetch, getApiErrorMessage } from '../utils/apiFetch.js'

// The ALKAIDUSB root device of USB forwarding on the doctor's own row (input.usb). The host answers GET /api/usbip-root/status and
// POST /api/usbip-root/repair only on Windows builds that own the root device; any other answer (404, 444, a closed connection) is an
// older host and the row stays as the host wrote it. The fix is a repair action the shared page (MDoctor) shows as its own button,
// confirms and reports as it does for any other item.
export const USB_ROOT_REPAIR = 'driver:usbip-root'
const USB_ITEM = 'input.usb'

const rootHealthy = (s) => s?.root_present === true && s?.usbip_healthy === true

export function createUsbRootDoctor({ t, fetcher = apiFetch } = {}) {
  /** The host's state of the root device, or null when it has none to tell (an older host, a sign-in lost, an answer of another shape). */
  async function status() {
    try {
      const r = await fetcher('/api/usbip-root/status')
      if (!r.ok) return null
      const j = await r.json().catch(() => null)
      if (!j || typeof j !== 'object' || !('root_present' in j || 'usbip_healthy' in j || j.error)) return null
      return j
    }
    catch { return null }
  }

  /** The answer of one doctor group with the root device's state on input.usb: healthy adds nothing, unhealthy turns the row into a warning with a repair. */
  function patch(json, s) {
    const items = json?.items
    const i = Array.isArray(items) ? items.findIndex((it) => it?.id === USB_ITEM) : -1
    if (i < 0 || items[i].status === 'skip') return json
    const item = { ...items[i] }
    const ds4 = s.ds4_fallback ? t('troubleshooting.usbip_root_ds4') : ''
    if (!rootHealthy(s) && !item.fix?.action) {
      item.status = item.status === 'fail' ? 'fail' : 'warn'
      // the two sentences share one line: the first one gives up its full stop
      const state = t('troubleshooting.usbip_root_unhealthy')
      item.detail = ds4 ? `${state.replace(/[。.]$/, '')} · ${ds4}` : state
      // level 2: the host service runs the helper itself, there is no Windows prompt on the person's screen (level 3 would say there is)
      item.fix = { text: '', action: { kind: 'repair', target: USB_ROOT_REPAIR, level: 2, effect: t('troubleshooting.usbip_root_effect') } }
    }
    else if (ds4) item.detail = [item.detail, ds4].filter(Boolean).join(' · ')
    else return json
    const next = items.slice()
    next[i] = item
    return { ...json, items: next }
  }

  /** The doctor's request: the input group gets the root device's state, every other path passes through. */
  function wrap(request) {
    return async (path) => {
      const r = await request(path)
      if (!r?.ok || !String(path).startsWith('/api/doctor?')) return r
      if (new URLSearchParams(String(path).split('?')[1]).get('only') !== 'input') return r
      if (!r.json?.items?.some?.((it) => it?.id === USB_ITEM)) return r
      const s = await status()
      return s ? { ...r, json: patch(r.json, s) } : r
    }
  }

  const canRepair = (action) => action?.target === USB_ROOT_REPAIR

  /** Runs the repair; resolves when the root device is healthy, else throws the host's reason (the page shows it on the row). */
  async function repair(action) {
    if (!canRepair(action)) throw new Error(String(action?.target || ''))
    const r = await fetcher('/api/usbip-root/repair', { method: 'POST' })
    const j = await r.json().catch(() => null)
    if (!r.ok) throw new Error(getApiErrorMessage(j || {}, r))
    if (!j || j.success === false) throw new Error(j?.error || t('troubleshooting.usbip_root_unhealthy'))
    if (j.usbip_healthy === false) throw new Error(t('troubleshooting.usbip_root_unhealthy'))
  }

  return { request: wrap, repair, canRepair, status }
}
