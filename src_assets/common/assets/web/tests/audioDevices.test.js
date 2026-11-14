import assert from 'node:assert/strict'
import { readFileSync, readdirSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import test, { after, before } from 'node:test'
import { startHarness } from './helpers/editorHarness.js'
import { FakeNode, findAll, textOf } from './helpers/fakeDom.js'
import {
  MANUAL_DEVICE, deviceName, findDevice, normalizeAudioDevices, pickerOptions, pickerText, pickerValue, usableDevices, vbCableErrorKind, vbCableReady, vbCableRunningStep,
} from '../utils/audioDevices.js'

const here = dirname(fileURLToPath(import.meta.url))
const foundationUi = process.env.FOUNDATION_UI_DIR || join(here, '../../../../../../ds5-ui-m3e')

// the small DOM of the harness has no <dialog>: the confirmation dialog only needs these two to open and close
FakeNode.prototype.showModal ||= function showModal() { this.open = true }
FakeNode.prototype.close ||= function close() { this.open = false }

// What the host answers on a machine like the VM (Steam Streaming Speakers default, another virtual card, VB-CABLE present but disabled).
const ANSWER = {
  success: true, version: 1, supported: true,
  render: [
    { id: '{0.0.0.00000000}.{steam}', name: 'Speakers (Steam Streaming Speakers)', adapter: 'Steam Streaming Speakers', description: 'Speakers', state: 'active', form_factor: 'speakers', default: true, default_communications: true, virtual: 'steam_streaming' },
    { id: '{0.0.0.00000000}.{cable}', name: 'CABLE Input (VB-Audio Virtual Cable)', adapter: 'VB-Audio Virtual Cable', description: 'CABLE Input', state: 'active', form_factor: 'line_level', default: false, default_communications: false, virtual: 'vb_cable' },
    { id: '{0.0.0.00000000}.{old}', name: 'Headphones (USB)', adapter: 'USB', description: 'Headphones', state: 'disabled', form_factor: 'headphones', default: false, default_communications: false, virtual: null },
  ],
  capture: [],
  vb_cable: { installed: true, state: 'ready', job: { action: 'install', step: 'done', running: false, error_code: '', detail: '' }, render_id: '{0.0.0.00000000}.{cable}', render_name: 'CABLE Input (VB-Audio Virtual Cable)', capture_id: 'x', capture_name: 'CABLE Output (VB-Audio Virtual Cable)' },
}
const LABELS = {
  auto: 'Auto', manual: 'Manual…', missing: 'Not found', defaultTag: 'Default',
  kinds: { vb_cable: 'VB-CABLE', steam_streaming: 'Steam' }, defaultNow: (name) => `Now: ${name}`,
}

test('the answer of the host becomes lists with the fields the pickers read', () => {
  const state = normalizeAudioDevices(ANSWER)
  assert.equal(state.supported, true)
  assert.equal(state.render.length, 3)
  assert.equal(state.render[0].isDefault, true)
  assert.equal(state.render[1].virtual, 'vb_cable')
  assert.equal(state.render[2].virtual, '')
  assert.equal(state.vb.state, 'ready')
  assert.equal(vbCableReady(state.vb), true)
})

test('the job of the host that installs or enables VB-CABLE is read as it is reported', () => {
  const running = normalizeAudioDevices({ ...ANSWER, vb_cable: { state: 'missing', job: { action: 'install', step: 'verifying', running: true, error_code: '', detail: '' } } })
  assert.equal(running.vb.job.running, true)
  assert.equal(vbCableRunningStep(running.vb.job), 'verifying')
  const failed = normalizeAudioDevices({ ...ANSWER, vb_cable: { state: 'missing', job: { action: 'enable', step: 'failed', running: false, error_code: 'VB_ENABLE_FAILED', detail: 'exit code 5' } } })
  assert.equal(failed.vb.job.errorCode, 'VB_ENABLE_FAILED')
  assert.equal(vbCableRunningStep(failed.vb.job), '')
  assert.equal(normalizeAudioDevices(null).vb.job.running, false)
  // an answer of an older host has no job: nothing is running
  assert.equal(normalizeAudioDevices({ success: true, supported: true, render: [], capture: [], vb_cable: { state: 'missing' } }).vb.job.running, false)
})

test('a failed job is told by what a person can do about it', () => {
  assert.equal(vbCableErrorKind('VB_DOWNLOAD_FAILED'), 'download')
  assert.equal(vbCableErrorKind('VB_EXTRACT_FAILED'), 'download')
  assert.equal(vbCableErrorKind('VB_SIGNATURE_INVALID'), 'signature')
  assert.equal(vbCableErrorKind('VB_SIGNER_UNEXPECTED'), 'signature')
  assert.equal(vbCableErrorKind('VB_NEEDS_ADMIN'), 'admin')
  for (const code of ['VB_INSTALL_FAILED', 'VB_INSTALL_TIMEOUT', 'VB_ENABLE_FAILED', 'VB_STILL_UNAVAILABLE']) assert.equal(vbCableErrorKind(code), 'failed', code)
  assert.equal(vbCableErrorKind(''), '')
})

test('an unsupported, failed or missing answer is an empty list, never a throw', () => {
  assert.equal(normalizeAudioDevices(null).render.length, 0)
  assert.equal(normalizeAudioDevices({ success: false }).supported, false)
  const linux = normalizeAudioDevices({ success: true, supported: false, render: [], capture: [], vb_cable: { state: 'unsupported' } })
  assert.equal(linux.supported, false)
  assert.equal(linux.vb.state, 'unsupported')
  assert.equal(vbCableReady(linux.vb), false)
})

test('a stored value finds its device by id, name, description or adapter, like the host', () => {
  const { render } = normalizeAudioDevices(ANSWER)
  assert.equal(findDevice(render, '{0.0.0.00000000}.{cable}').virtual, 'vb_cable')
  assert.equal(findDevice(render, 'CABLE Input (VB-Audio Virtual Cable)').virtual, 'vb_cable')
  assert.equal(findDevice(render, 'CABLE Input').virtual, 'vb_cable')
  assert.equal(findDevice(render, 'Steam Streaming Speakers').id, '{0.0.0.00000000}.{steam}')
  assert.equal(findDevice(render, 'cable input').virtual, 'vb_cable')
  assert.equal(findDevice(render, 'Nothing like it'), null)
  assert.equal(findDevice(render, ''), null)
})

test('the row shows the name of the device, the stored text when the list does not know it, and auto when empty', () => {
  const { render } = normalizeAudioDevices(ANSWER)
  assert.equal(deviceName(render, '{0.0.0.00000000}.{steam}'), 'Speakers (Steam Streaming Speakers)')
  assert.equal(deviceName(render, 'Old Card'), 'Old Card')
  assert.equal(pickerText(render, '', 'Auto'), 'Auto')
  assert.equal(pickerText(render, 'CABLE Input', 'Auto'), 'CABLE Input (VB-Audio Virtual Cable)')
})

test('the picker lists auto, every usable device, VB-CABLE labelled, and the way to type one', () => {
  const { render } = normalizeAudioDevices(ANSWER)
  const options = pickerOptions(render, '', LABELS)
  assert.deepEqual(options.map((o) => o.value), ['', '{0.0.0.00000000}.{steam}', '{0.0.0.00000000}.{cable}', MANUAL_DEVICE])
  assert.equal(options[0].label, 'Auto')
  assert.equal(options[0].text, 'Now: Speakers (Steam Streaming Speakers)')
  assert.equal(options[1].text, 'Steam · Default')
  assert.equal(options[2].text, 'VB-CABLE')
  assert.equal(options.at(-1).label, 'Manual…')
  assert.equal(usableDevices(render).length, 2, 'a disabled device is not offered')
})

test('a stored value that no device matches stays in the list and says so', () => {
  const { render } = normalizeAudioDevices(ANSWER)
  const options = pickerOptions(render, 'Gone Card', LABELS)
  const stored = options.find((o) => o.value === 'Gone Card')
  assert.ok(stored)
  assert.equal(stored.text, 'Not found')
  assert.equal(pickerValue(render, 'Gone Card'), 'Gone Card')
  // a stored name that a device matches is not repeated, it is that device
  assert.equal(pickerOptions(render, 'CABLE Input', LABELS).filter((o) => o.label === 'CABLE Input').length, 0)
  assert.equal(pickerValue(render, 'CABLE Input'), '{0.0.0.00000000}.{cable}')
})

test('with no list (other systems, host not reached) the picker still offers auto, a stored value and typing', () => {
  const options = pickerOptions([], 'alsa_output.pci', LABELS)
  assert.deepEqual(options.map((o) => o.value), ['', 'alsa_output.pci', MANUAL_DEVICE])
})

// The rows on a real page of the Web UI, drawn on the small DOM of the editor harness: the copy keys show instead of text because the harness loads no messages.
let h
let CfgDevice
let VbCableRow
before(async () => {
  h = await startHarness()
  CfgDevice = (await h.load('/components/cfg/CfgDevice.vue')).default
  VbCableRow = (await h.load('/components/cfg/VbCableRow.vue')).default
})
after(async () => {
  await h?.close()
  setTimeout(() => process.exit(process.exitCode ?? 0), 800).unref()
})

test('the device row shows the chosen device by name and automatic when nothing is stored', async () => {
  const { render } = normalizeAudioDevices(ANSWER)
  const chosen = await h.mount(CfgDevice, { config: { audio_sink: '{0.0.0.00000000}.{cable}' }, k: 'audio_sink', devices: render })
  assert.match(chosen.text(), /CABLE Input \(VB-Audio Virtual Cable\)/)
  chosen.unmount()
  const auto = await h.mount(CfgDevice, { config: { audio_sink: '' }, k: 'audio_sink', devices: render })
  assert.match(auto.text(), /_common\.auto/)
  auto.unmount()
})

test('the expert device row is an expert row with its own control, not a full-width button', async () => {
  const { render } = normalizeAudioDevices(ANSWER)
  const row = await h.mount(CfgDevice, { config: { virtual_sink: 'Speakers (Steam Streaming Speakers)' }, k: 'virtual_sink', devices: render, expert: true })
  assert.match(row.text(), /Speakers \(Steam Streaming Speakers\)/)
  assert.equal(findAll(row.root, (n) => n.attrs?.class?.includes('m-expert--stack')).length, 0)
  row.unmount()
})

test('the VB-CABLE row names the state and offers the download unless it is ready or only disabled', async () => {
  for (const [state, download] of [['ready', false], ['partial', true], ['unavailable', true], ['missing', true], ['disabled', false]]) {
    const row = await h.mount(VbCableRow, { vb: { state }, loaded: true })
    assert.match(row.text(), new RegExp(`config\\.vb_cable_state_${state}`), state)
    assert.equal(row.buttons('_common.download').length > 0, download, `${state}: download entry`)
    row.unmount()
  }
  const loading = await h.mount(VbCableRow, { vb: { state: 'missing' }, loaded: false })
  assert.equal(loading.buttons('_common.download').length, 0, 'no verdict before the list is read')
  loading.unmount()
})

test('the VB-CABLE row offers to install what is missing and to enable what is disabled, and says what the job is doing', async () => {
  for (const [state, action] of [['missing', 'install'], ['unavailable', 'install'], ['partial', 'install'], ['disabled', 'enable']]) {
    const row = await h.mount(VbCableRow, { vb: { state }, loaded: true })
    assert.equal(row.buttons(`config.vb_cable_${action}`).length, 1, `${state}: ${action}`)
    assert.equal(row.buttons(`config.vb_cable_${action === 'install' ? 'enable' : 'install'}`).length, 0, `${state}: not the other one`)
    row.unmount()
  }
  const ready = await h.mount(VbCableRow, { vb: { state: 'ready' }, loaded: true })
  assert.equal(ready.buttons('config.vb_cable_install').length + ready.buttons('config.vb_cable_enable').length, 0)
  ready.unmount()
  const running = await h.mount(VbCableRow, { vb: { state: 'missing', job: { running: true, step: 'installing' } }, loaded: true })
  assert.match(running.text(), /config\.vb_cable_job_installing/)
  running.unmount()
  const failed = await h.mount(VbCableRow, { vb: { state: 'disabled', job: { running: false, step: 'failed', errorCode: 'VB_NEEDS_ADMIN' } }, loaded: true })
  assert.match(failed.text(), /config\.vb_cable_err_admin/)
  failed.unmount()
  const stale = await h.mount(VbCableRow, { vb: { state: 'ready', job: { running: false, step: 'failed', errorCode: 'VB_NEEDS_ADMIN' } }, loaded: true })
  assert.doesNotMatch(stale.text(), /vb_cable_err/, 'a verdict of an old job is not shown once the device works')
  stale.unmount()
})

test('nothing is sent to the host until the person confirms, and the confirmation says what will happen', async () => {
  const sent = []
  const realFetch = globalThis.fetch
  globalThis.fetch = async (url, options) => { sent.push({ url, method: options?.method, body: options?.body }); return { ok: true, json: async () => ({ success: true, started: true }) } }
  try {
    const row = await h.mount(VbCableRow, { vb: { state: 'missing' }, loaded: true }, { changed: () => sent.push('changed') })
    await row.press(row.buttons('config.vb_cable_install')[0])
    assert.match(row.text(), /config\.vb_cable_install_confirm/, 'the dialog says what the host will do')
    assert.deepEqual(sent, [], 'asking is not doing')
    await row.press(row.button('_common.cancel'))
    assert.deepEqual(sent, [], 'cancel sends nothing')
    await row.press(row.buttons('config.vb_cable_install')[0])
    await row.press(row.buttons('config.vb_cable_install').at(-1))
    await row.settle()
    const post = sent.find((s) => typeof s === 'object')
    assert.ok(post, 'confirming sends the request')
    assert.match(String(post.url), /\/api\/audio\/vb-cable\/install$/)
    assert.equal(post.method, 'POST')
    assert.deepEqual(JSON.parse(post.body), { confirm: true })
    assert.ok(sent.includes('changed'), 'the page is told to read the progress')
    row.unmount()
    sent.length = 0
    const disabled = await h.mount(VbCableRow, { vb: { state: 'disabled' }, loaded: true })
    await disabled.press(disabled.buttons('config.vb_cable_enable')[0])
    assert.match(disabled.text(), /config\.vb_cable_enable_confirm/)
    await disabled.press(disabled.buttons('config.vb_cable_enable').at(-1))
    await disabled.settle()
    assert.match(String(sent.find((s) => typeof s === 'object')?.url), /\/api\/audio\/vb-cable\/enable$/)
    disabled.unmount()
  } finally {
    globalThis.fetch = realFetch
  }
})

test('a disabled VB-CABLE offers the Device Manager inside the control panel, and nothing a browser cannot do outside it', async () => {
  const had = Object.getOwnPropertyDescriptor(globalThis, 'window')
  try {
    globalThis.window = { isTauri: true }
    const inPanel = await h.mount(VbCableRow, { vb: { state: 'disabled' }, loaded: true })
    assert.match(inPanel.text(), /config\.vb_cable_state_disabled/)
    assert.equal(inPanel.buttons('config.vb_cable_open_devmgmt').length, 1)
    assert.equal(inPanel.buttons('_common.download').length, 0, 'not a download')
    inPanel.unmount()
    globalThis.window = {}
    const inBrowser = await h.mount(VbCableRow, { vb: { state: 'disabled' }, loaded: true })
    assert.equal(inBrowser.buttons('config.vb_cable_open_devmgmt').length, 0)
    assert.equal(inBrowser.buttons('_common.download').length, 0)
    inBrowser.unmount()
    // the other states never offer the Device Manager
    globalThis.window = { isTauri: true }
    const missing = await h.mount(VbCableRow, { vb: { state: 'missing' }, loaded: true })
    assert.equal(missing.buttons('config.vb_cable_open_devmgmt').length, 0)
    missing.unmount()
  } finally {
    if (had) Object.defineProperty(globalThis, 'window', had); else delete globalThis.window
  }
})

test('the copy of the device pickers is translated in every language file, and the old command hint is gone from the Windows text', () => {
  const dir = new URL('../public/assets/locale/', import.meta.url)
  const load = (file) => JSON.parse(readFileSync(new URL(`${file}.json`, dir), 'utf8')).config
  const en = load('en')
  const keys = Object.keys(en).filter((k) => /^(audio_device_|vb_cable_|route_mic_backend_|vb_cable_open_devmgmt)/.test(k))
  assert.ok(keys.length >= 33, `${keys.length} keys`)
  // names that are the same word in every language
  const SAME = new Set(['vb_cable_title', 'route_mic_backend_vb_cable'])
  const ENGLISH = new Set(['en', 'en_GB', 'en_US'])
  const files = readdirSync(dir).filter((f) => f.endsWith('.json')).map((f) => f.replace(/\.json$/, ''))
  assert.ok(files.length >= 20)
  for (const file of files) {
    const config = load(file)
    for (const key of [...keys, 'audio_sink_desc_windows']) {
      assert.equal(typeof config[key], 'string', `${file}.json lacks ${key}`)
      if (!ENGLISH.has(file) && !SAME.has(key)) assert.notEqual(config[key], en[key], `${file}.json leaves ${key} in English`)
    }
    for (const key of ['audio_sink', 'virtual_sink', 'keep_sink_default', 'microphone_redirect_backend']) {
      if (!ENGLISH.has(file)) assert.notEqual(config[key], en[key], `${file}.json leaves the title ${key} in English`)
    }
    assert.doesNotMatch(config.audio_sink_desc_windows, /audio-info/i, `${file}.json still points at audio-info`)
    assert.match(config.audio_device_default_now, /\{name\}/, `${file}.json lost the {name} placeholder`)
  }
  // the two states that tell a person what to do name the Device Manager or the missing driver
  assert.match(load('zh').vb_cable_state_disabled, /已安装但已在设备管理器中禁用/)
  assert.match(load('zh').vb_cable_open_devmgmt, /设备管理器/)
  // the names the design asks for
  const zh = load('zh')
  assert.equal(zh.audio_sink, '采集哪个扬声器')
  assert.equal(zh.virtual_sink, '串流时切换到的扬声器')
  assert.equal(zh.keep_sink_default, '锁定扬声器')
  assert.equal(zh.microphone_redirect_backend, '麦克风方式')
})

// The original shows these up front on its audio page (AudioVideo.vue, windows block): they are rows of the page here too, not expert rows.
test('the speaker settings the original shows up front are not behind the advanced section', () => {
  const page = readFileSync(new URL('../configs/tabs/Audio.vue', import.meta.url), 'utf8')
  const sound = page.slice(page.indexOf('<CfgSub id="sound"'), page.indexOf('<CfgSub id="microphone"'))
  for (const key of ['audio_sink', 'virtual_sink', 'keep_sink_default', 'install_steam_audio_drivers']) assert.match(sound, new RegExp(`k="${key}"`), `${key} is on the audio page`)
  assert.doesNotMatch(sound, /tabs\.advanced/, 'no advanced section hides them')
  assert.doesNotMatch(sound, /CfgExpert/)
  const meta = readFileSync(join(foundationUi, 'src/settings/meta.js'), 'utf8')
  for (const key of ['virtual_sink', 'keep_sink_default', 'install_steam_audio_drivers']) assert.match(meta, new RegExp(`${key}: [FS]\\(2,`), `${key} is a tier 2 setting`)
})

test('every microphone mode the host offers is a button of the microphone page', () => {
  const page = readFileSync(new URL('../configs/tabs/Audio.vue', import.meta.url), 'utf8')
  for (const mode of ['auto', 'usbip_experimental', 'vb_cable', 'disabled']) assert.ok(page.includes(`'${mode}'`), mode)
  assert.match(page, /k="microphone_redirect_backend"/)
  assert.match(page, /\['auto', 'usbip_experimental', 'vb_cable', 'disabled'\]\.map/)
})
