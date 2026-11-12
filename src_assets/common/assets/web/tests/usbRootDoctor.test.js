import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { dirname, join } from 'node:path'
import { createDoctor, doctorTexts, fixButton } from 'foundation-ui/doctor'
import { createUsbRootDoctor, USB_ROOT_REPAIR } from '../composables/useUsbRootDoctor.js'

const here = dirname(fileURLToPath(import.meta.url))
const WORDS = {
  'troubleshooting.usbip_root_unhealthy': 'root unavailable',
  'troubleshooting.usbip_root_ds4': 'running as DS4',
  'troubleshooting.usbip_root_effect': 'repairs the root device',
}
const t = (key) => WORDS[key] ?? key

const reply = (status, body) => new Response(body === undefined ? null : JSON.stringify(body), { status })
const inputGroup = (usb) => ({
  version: 1,
  groups: [{ id: 'input', title: 'Input', items: ['input.vigem', 'input.usb'] }],
  items: [
    { id: 'input.vigem', group: 'input', status: 'pass', title: 'ViGEmBus', detail: 'ok' },
    { id: 'input.usb', group: 'input', status: 'pass', title: 'USB forwarding', detail: 'usbip-win2 ready', ...usb },
  ],
})
// a host: the doctor's input group, the root device's status route (a Response, or a function), the repair route
const host = ({ status, repair, group = inputGroup({}) } = {}) => {
  const calls = []
  const fetcher = async (url, options = {}) => {
    calls.push(`${options.method || 'GET'} ${url}`)
    const answer = url === '/api/usbip-root/status' ? status : url === '/api/usbip-root/repair' ? repair : undefined
    if (answer === 'drop') throw new TypeError('connection closed')
    const r = typeof answer === 'function' ? answer() : answer
    return r ?? reply(404, { error: 'not found' })
  }
  const request = async (path) => ({ ok: true, status: 200, json: structuredClone(group), path })
  return { fetcher, request, calls }
}
const usbOf = async (h) => {
  const doc = createUsbRootDoctor({ t, fetcher: h.fetcher })
  const r = await doc.request(h.request)('/api/doctor?only=input&lang=en')
  return { doc, item: r.json.items.find((i) => i.id === 'input.usb'), all: r.json.items }
}
const healthy = { root_present: true, usbip_healthy: true, ds4_fallback: '' }
const broken = { root_present: false, usbip_healthy: false, ds4_fallback: '' }

test("healthy: the row is the host's own, nothing is added", async () => {
  const h = host({ status: reply(200, healthy) })
  const { item } = await usbOf(h)
  assert.equal(item.status, 'pass')
  assert.equal(item.detail, 'usbip-win2 ready')
  assert.equal(item.fix?.action, undefined)
  assert.deepEqual(h.calls, ['GET /api/usbip-root/status'])
})

test('unhealthy: one line of state and a repair on the USB row, the other items stay', async () => {
  const h = host({ status: reply(200, broken) })
  const { item, all } = await usbOf(h)
  assert.equal(item.status, 'warn')
  assert.equal(item.detail, 'root unavailable')
  assert.equal(item.fix.text, '')
  assert.deepEqual(item.fix.action, { kind: 'repair', target: USB_ROOT_REPAIR, level: 2, effect: 'repairs the root device' })
  assert.equal(all.find((i) => i.id === 'input.vigem').status, 'pass')
})

test('the shared page shows the repair as its own fix button on that row, with the confirm of every repair', async () => {
  const h = host({ status: reply(200, broken) })
  const doc = createUsbRootDoctor({ t, fetcher: h.fetcher })
  const doctor = createDoctor({ request: doc.request(h.request), locale: 'en' })
  await doctor.rerun('input')
  const row = doctor.state.groups.find((g) => g.id === 'input').items.find((i) => i.id === 'input.usb')
  assert.equal(row.status, 'warn')
  assert.equal(row.fix.action.level, 2)
  assert.equal(row.fix.action.confirm, true)
  const T = doctorTexts('zh')
  assert.ok(fixButton(row.fix.action, T, { canRepair: doc.canRepair }), "a button with this product's repair")
  assert.equal(fixButton(row.fix.action, T, { canRepair: () => false }), null)
  assert.equal(doc.canRepair({ kind: 'repair', target: 'driver:usbip' }), false, "the installs stay the control panel's")
})

test('ds4_fallback: one more line that the pad runs as DS4, on a healthy row and on an unhealthy one', async () => {
  const ok = await usbOf(host({ status: reply(200, { ...healthy, ds4_fallback: 'DS4 fallback remains active' }) }))
  assert.equal(ok.item.status, 'pass')
  assert.equal(ok.item.detail, 'usbip-win2 ready · running as DS4')
  assert.equal(ok.item.fix?.action, undefined)
  const bad = await usbOf(host({ status: reply(200, { ...broken, ds4_fallback: 'DS4 fallback remains active' }) }))
  assert.equal(bad.item.detail, 'root unavailable · running as DS4')
  assert.equal(bad.item.fix.action.target, USB_ROOT_REPAIR)
})

test('repair success: the page gets a resolved repair and asks for the group again', async () => {
  const h = host({ status: reply(200, broken), repair: reply(200, { ...healthy, success: true }) })
  const { doc } = await usbOf(h)
  await doc.repair({ kind: 'repair', target: USB_ROOT_REPAIR })
  assert.deepEqual(h.calls.slice(-1), ['POST /api/usbip-root/repair'])
})

test("repair failure: the host's reason reaches the row", async () => {
  const doc = createUsbRootDoctor({ t, fetcher: host({ repair: reply(200, { success: false, error: 'helper not installed', ds4_fallback: 'x' }) }).fetcher })
  await assert.rejects(doc.repair({ kind: 'repair', target: USB_ROOT_REPAIR }), /helper not installed/)
  const http = createUsbRootDoctor({ t, fetcher: host({ repair: reply(500, { error: 'access denied' }) }).fetcher })
  await assert.rejects(http.repair({ kind: 'repair', target: USB_ROOT_REPAIR }), /access denied/)
  const still = createUsbRootDoctor({ t, fetcher: host({ repair: reply(200, { success: true, root_present: true, usbip_healthy: false }) }).fetcher })
  await assert.rejects(still.repair({ kind: 'repair', target: USB_ROOT_REPAIR }), /root unavailable/)
  const down = createUsbRootDoctor({ t, fetcher: host({ repair: 'drop' }).fetcher })
  await assert.rejects(down.repair({ kind: 'repair', target: USB_ROOT_REPAIR }), /connection closed/)
  await assert.rejects(doc.repair({ kind: 'repair', target: 'driver:usbip' }), /driver:usbip/)
})

test('an older host (404, 444, a closed connection, another shape, a lost sign-in) shows nothing', async () => {
  for (const status of [reply(404, { error: 'no' }), reply(444), reply(401, { error: 'auth' }), reply(200, { hello: 'world' }), 'drop']) {
    const { item } = await usbOf(host({ status }))
    assert.equal(item.status, 'pass', String(status))
    assert.equal(item.detail, 'usbip-win2 ready')
    assert.equal(item.fix?.action, undefined)
  }
})

test('only the input group asks, and a host row that already has a fix is left alone', async () => {
  const other = host({ status: reply(200, broken) })
  const doc = createUsbRootDoctor({ t, fetcher: other.fetcher })
  await doc.request(other.request)('/api/doctor?only=gpu&lang=en')
  await doc.request(other.request)('/api/runtime/sessions')
  assert.deepEqual(other.calls, [])
  const withFix = { status: 'warn', detail: 'usbip-win2 is not installed', fix: { text: 'Install it', action: { kind: 'repair', target: 'driver:usbip', level: 3, confirm: true, effect: '' } } }
  const { item } = await usbOf(host({ status: reply(200, broken), group: inputGroup(withFix) }))
  assert.equal(item.detail, 'usbip-win2 is not installed')
  assert.equal(item.fix.action.target, 'driver:usbip')
  const skip = await usbOf(host({ status: reply(200, broken), group: inputGroup({ status: 'skip', detail: 'Windows only' }) }))
  assert.equal(skip.item.status, 'skip')
})

test('the doctor page uses it and the Bootstrap card is gone', () => {
  const page = readFileSync(join(here, '../views/Doctor.vue'), 'utf8')
  assert.match(page, /:repair="usbRoot\.repair"/)
  assert.match(page, /:can-repair="usbRoot\.canRepair"/)
  assert.doesNotMatch(page, /UsbipRootHealthCard/)
  for (const lang of ['en', 'zh']) {
    const text = JSON.parse(readFileSync(join(here, `../public/assets/locale/${lang}.json`), 'utf8')).troubleshooting
    for (const key of ['usbip_root_unhealthy', 'usbip_root_ds4', 'usbip_root_effect']) assert.ok(text[key], `${lang} ${key}`)
    // the row says what is wrong in words a person can act on: no device-internal names, and it says what stops working
    for (const key of ['usbip_root_unhealthy', 'usbip_root_effect']) assert.doesNotMatch(text[key], /ALKAIDUSB|VHCI|usbip|根设备|root device/i, `${lang} ${key} names an internal part`)
    assert.match(text.usbip_root_unhealthy, lang === 'zh' ? /虚拟 USB 设备.*DualSense/ : /virtual USB device.*DualSense/, `${lang} usbip_root_unhealthy says what stops working`)
    for (const key of ['usbip_root_title', 'usbip_root_healthy', 'usbip_root_repair', 'usbip_root_repairing', 'usbip_root_recheck']) assert.equal(text[key], undefined, `${lang} ${key}`)
  }
})
