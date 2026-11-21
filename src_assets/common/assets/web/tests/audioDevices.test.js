import assert from 'node:assert/strict'
import { readFileSync, readdirSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import test, { after, before } from 'node:test'
import { startHarness } from './helpers/editorHarness.js'
import { FakeNode, findAll, textOf } from './helpers/fakeDom.js'
import {
  MANUAL_DEVICE, deviceName, findDevice, looksLikeEndpointId, noDefaultOutputNotice, normalizeAudioDevices, pickerOptions, pickerText, pickerValue, steamDriverState, usableDevices, vbCableReady,
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
  auto: 'Auto', manual: 'Manual…', missing: 'Not found', unknown: 'Selected device', defaultTag: 'Default',
  kinds: { vb_cable: 'VB-CABLE', steam_streaming: 'Steam' }, states: { disabled: 'Disabled', unplugged: 'Unplugged' },
  vb: { missing: 'Not installed', disabled: 'Disabled in Device Manager', unavailable: 'Not usable now', partial: 'Not usable now' },
  defaultNow: (name) => `Now: ${name}`,
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

test('the picker lists auto, every usable device, VB-CABLE labelled, the unusable ones with the reason, and the way to type one', () => {
  const { render } = normalizeAudioDevices(ANSWER)
  const options = pickerOptions(render, '', LABELS)
  assert.deepEqual(options.map((o) => o.value), ['', '{0.0.0.00000000}.{steam}', '{0.0.0.00000000}.{cable}', '{0.0.0.00000000}.{old}', MANUAL_DEVICE])
  assert.equal(options[0].label, 'Auto')
  assert.equal(options[0].text, 'Now: Speakers (Steam Streaming Speakers)')
  assert.equal(options[1].text, 'Steam · Default')
  assert.equal(options[2].text, 'VB-CABLE')
  assert.equal(options.at(-1).label, 'Manual…')
  assert.equal(usableDevices(render).length, 2, 'a disabled device is not usable')
  // a disabled device is in the list with its reason and cannot be chosen
  assert.equal(options[3].label, 'Headphones (USB)')
  assert.equal(options[3].text, 'Disabled')
  assert.equal(options[3].disabled, true)
  assert.equal(options.filter((o) => o.disabled).length, 1)
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
let NoDefaultOutputNotice
before(async () => {
  h = await startHarness()
  CfgDevice = (await h.load('/components/cfg/CfgDevice.vue')).default
  VbCableRow = (await h.load('/components/cfg/VbCableRow.vue')).default
  NoDefaultOutputNotice = (await h.load('/components/cfg/NoDefaultOutputNotice.vue')).default
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

test('the VB-CABLE row names the state and offers no download; management lives in the Panel', async () => {
  for (const state of ['ready', 'partial', 'unavailable', 'missing', 'disabled']) {
    const row = await h.mount(VbCableRow, { vb: { state }, loaded: true })
    assert.match(row.text(), new RegExp(`config\\.vb_cable_state_${state}`), state)
    assert.equal(row.buttons('_common.download').length, 0, `${state}: no download entry`)
    row.unmount()
  }
  const loading = await h.mount(VbCableRow, { vb: { state: 'missing' }, loaded: false })
  assert.equal(loading.buttons('_common.download').length, 0, 'no verdict before the list is read')
  loading.unmount()
})

test('VB-CABLE management does not post driver operations from the web UI', async () => {
  for (const state of ['ready', 'partial', 'disabled', 'unavailable', 'missing']) {
    const row = await h.mount(VbCableRow, { vb: { state }, loaded: true })
    assert.equal(row.buttons('config.vb_cable_install').length, 0, state)
    assert.equal(row.buttons('config.vb_cable_enable').length, 0, state)
    row.unmount()
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
    const unavailable = await h.mount(VbCableRow, { vb: { state: 'unavailable' }, loaded: true })
    assert.equal(unavailable.buttons('config.vb_cable_open_devmgmt').length, 1)
    assert.equal(unavailable.buttons('config.vb_cable_install').length, 0)
    unavailable.unmount()
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
  assert.equal(zh.audio_sink, '目标采集扬声器')
  assert.equal(zh.virtual_sink, '串流时的虚拟扬声器')
  assert.equal(zh.keep_sink_default, '锁定默认扬声器')
  assert.equal(zh.microphone_redirect_backend, '客户端麦克风去向')
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

// ids and names as the owner's Windows shows them: Chinese friendly names, an id the list lost, a name that is GBK read as UTF-8
const CABLE_ID = '{0.0.0.00000000}.{172f61d5-5aff-4b19-9233-e02c0003996f}'
const STEAM_ID = '{0.0.0.00000000}.{32805c76-6b36-4e36-9c06-833720385d20}'
const chinese = (extra = []) => normalizeAudioDevices({
  ...ANSWER,
  render: [
    { id: STEAM_ID, name: '扬声器 (Steam Streaming Speakers)', adapter: 'Steam Streaming Speakers', description: '扬声器', state: 'active', form_factor: 'speakers', default: true, default_communications: true, virtual: 'steam_streaming' },
    { id: CABLE_ID, name: 'CABLE Input (VB-Audio Virtual Cable)', adapter: 'VB-Audio Virtual Cable', description: 'CABLE Input', state: 'active', form_factor: 'speakers', default: false, default_communications: false, virtual: 'vb_cable' },
    ...extra,
  ],
}).render

test('an endpoint id is told from a device name, in every language of Windows', () => {
  assert.equal(looksLikeEndpointId(CABLE_ID), true)
  assert.equal(looksLikeEndpointId(`  ${STEAM_ID.toUpperCase()} `), true)
  for (const name of ['CABLE Input (VB-Audio Virtual Cable)', '扬声器 (Steam Streaming Speakers)', '耳机 (USB Audio)', 'スピーカー', 'ϵͳ', '', '{0.0.0.00000000}', '{my speakers}']) assert.equal(looksLikeEndpointId(name), false, name)
})

test('a chosen device shows by its Chinese name, never as its id', () => {
  const render = chinese()
  assert.equal(deviceName(render, CABLE_ID), 'CABLE Input (VB-Audio Virtual Cable)')
  assert.equal(deviceName(render, STEAM_ID), '扬声器 (Steam Streaming Speakers)')
  assert.equal(pickerText(render, CABLE_ID, '自动', '所选设备'), 'CABLE Input (VB-Audio Virtual Cable)')
  // typed by the person, by the description or by the adapter: found as well
  assert.equal(deviceName(render, '扬声器'), '扬声器 (Steam Streaming Speakers)')
  assert.equal(deviceName(render, 'steam streaming speakers'), '扬声器 (Steam Streaming Speakers)')
})

test('an id that no device has any more is not shown: a word stands in for it', () => {
  const render = chinese()
  const lost = '{0.0.0.00000000}.{6f1a52f6-2c0d-4dd0-a2a0-0c3f7e9b8a11}'
  assert.equal(deviceName(render, lost, '所选设备'), '所选设备')
  assert.equal(pickerText(render, lost, '自动', '所选设备'), '所选设备')
  assert.equal(pickerText([], CABLE_ID, '自动', '所选设备'), '所选设备', 'the list could not be read: still no id on the row')
  const options = pickerOptions(render, lost, { ...LABELS, unknown: '所选设备' })
  const stored = options.find((o) => o.value === lost)
  assert.equal(stored.label, '所选设备')
  assert.equal(stored.text, 'Not found')
  assert.ok(!options.some((o) => o.label === lost), 'the id is the value of the entry, never its label')
  // a name typed by hand that matches nothing stays as typed
  assert.equal(deviceName(render, '耳机 (USB Audio)', '所选设备'), '耳机 (USB Audio)')
})

test('a name that is GBK read as UTF-8 stays a name: it is not mistaken for an id and not swapped for another', () => {
  // 系统 is CF B5 CD B3 in GBK, which is also valid UTF-8 and reads as Greek: a host that mixed it up would hand this over
  const render = chinese([{ id: '{0.0.0.00000000}.{aa000000-0000-4000-8000-000000000001}', name: 'ϵͳ (USB)', adapter: 'USB', description: 'ϵͳ', state: 'active', form_factor: 'speakers', default: false, default_communications: false, virtual: '' }])
  assert.equal(deviceName(render, 'ϵͳ (USB)'), 'ϵͳ (USB)')
  assert.equal(pickerText(render, 'ϵͳ', '自动', '所选设备'), 'ϵͳ (USB)')
  assert.equal(looksLikeEndpointId('ϵͳ (USB)'), false)
  const options = pickerOptions(render, '', LABELS)
  assert.equal(options.find((o) => o.label === 'ϵͳ (USB)').value, '{0.0.0.00000000}.{aa000000-0000-4000-8000-000000000001}')
})

test('an id and a name that point at the same device are one entry, whichever is stored', () => {
  const render = chinese()
  for (const stored of [CABLE_ID, 'CABLE Input (VB-Audio Virtual Cable)', 'CABLE Input']) {
    assert.equal(pickerValue(render, stored), CABLE_ID, stored)
    assert.equal(pickerOptions(render, stored, LABELS).filter((o) => o.value === CABLE_ID).length, 1, stored)
    assert.equal(pickerOptions(render, stored, LABELS).length, 4, `${stored}: auto, two devices, typing`)
  }
})

test('a disabled device is listed with its reason and stays the stored choice when it is one', () => {
  const render = chinese([{ id: '{0.0.0.00000000}.{bb000000-0000-4000-8000-000000000002}', name: '耳机 (USB Audio)', adapter: 'USB Audio', description: '耳机', state: 'unplugged', form_factor: 'headphones', default: false, default_communications: false, virtual: '' }])
  const options = pickerOptions(render, '{0.0.0.00000000}.{bb000000-0000-4000-8000-000000000002}', LABELS)
  const row = options.find((o) => o.label === '耳机 (USB Audio)')
  assert.equal(row.text, 'Unplugged')
  assert.equal(row.disabled, true)
  assert.equal(options.filter((o) => o.label === '耳机 (USB Audio)').length, 1, 'the stored id is not added a second time as a lost device')
  assert.equal(deviceName(render, row.value), '耳机 (USB Audio)')
})

test('VB-CABLE that cannot be used is in the list with the reason, once it is usable it is just a device', () => {
  const steamOnly = chinese().filter((d) => d.virtual !== 'vb_cable')
  for (const [state, text] of [['missing', 'Not installed'], ['disabled', 'Disabled in Device Manager'], ['unavailable', 'Not usable now']]) {
    const row = pickerOptions(steamOnly, '', LABELS, { state }).find((o) => o.label === 'VB-CABLE')
    assert.equal(row.text, text, state)
    assert.equal(row.disabled, true, state)
  }
  assert.ok(!pickerOptions(chinese(), '', LABELS, { state: 'ready' }).some((o) => o.disabled), 'a ready VB-CABLE is the ordinary entry')
  assert.ok(!pickerOptions(steamOnly, '', LABELS, null).some((o) => o.label === 'VB-CABLE'), 'without the summary of the host nothing is guessed')
})

test('the Steam driver is told from the devices it adds', () => {
  assert.equal(steamDriverState(chinese()), 'ready')
  assert.equal(steamDriverState(chinese().filter((d) => d.virtual !== 'steam_streaming')), 'missing')
  const disabled = chinese().map((d) => (d.virtual === 'steam_streaming' ? { ...d, state: 'disabled' } : d))
  assert.equal(steamDriverState(disabled), 'disabled')
  assert.equal(steamDriverState([]), 'missing')
})

test('the shell declares the device list before it hands it to the pages (a const read before its line breaks the whole page)', () => {
  const shell = readFileSync(new URL('../views/WebShell.vue', import.meta.url), 'utf8')
  const declared = shell.indexOf('const audioDevices = useAudioDevices(')
  const used = shell.indexOf('audio: audioDevices.render')
  assert.ok(declared > 0 && used > 0, 'both are there')
  assert.ok(declared < used, 'declared before the provide that reads it')
  assert.ok(declared < shell.indexOf('audio: audioDevices.render', shell.indexOf('useSettingsList(')), 'and before the settings list')
})

// A computer with no playback device at all (a server, a cloud machine, a VM): the host says so, the page explains and keeps looking.
const NO_DEVICE = { success: true, version: 1, supported: true, render: [], capture: [], default_output: { present: false, reason: 'no_endpoint', render_active: 0, host_waiting: true }, vb_cable: { installed: false, state: 'missing' } }

test('no default playback device becomes a notice with its reason and whether a stream waits for a device', () => {
  const state = normalizeAudioDevices(NO_DEVICE)
  assert.deepEqual(state.defaultOutput, { present: false, reason: 'no_endpoint', hostWaiting: true })
  assert.deepEqual(noDefaultOutputNotice(state), { reason: 'no_endpoint', hostWaiting: true })
  const waiting = normalizeAudioDevices({ ...NO_DEVICE, default_output: { present: false, reason: 'none_active', host_waiting: false } })
  assert.deepEqual(noDefaultOutputNotice(waiting), { reason: 'none_active', hostWaiting: false })
  // a reason this page does not know still gets a plain sentence, not a missing text
  assert.equal(noDefaultOutputNotice(normalizeAudioDevices({ ...NO_DEVICE, default_output: { present: false, reason: 'something_new' } })).reason, 'unknown')
})

test('the notice is silent while the list is loading, after a failed read, on another system, and when a device is the default', () => {
  const state = normalizeAudioDevices(NO_DEVICE)
  assert.equal(noDefaultOutputNotice(state, false), null)
  assert.equal(noDefaultOutputNotice(normalizeAudioDevices(null)), null)
  assert.equal(noDefaultOutputNotice(normalizeAudioDevices({ success: true, supported: false, render: [], capture: [] })), null)
  assert.equal(noDefaultOutputNotice(normalizeAudioDevices(ANSWER)), null)
  // an older host has no default_output: nothing is said on a guess
  const older = { ...ANSWER }
  delete older.default_output
  assert.equal(noDefaultOutputNotice(normalizeAudioDevices(older)), null)
  assert.equal(normalizeAudioDevices(older).defaultOutput.present, true)
})

test('the notice names VB-CABLE and Steam Streaming Speakers in every locale it is written for', () => {
  const dir = new URL('../public/assets/locale/', import.meta.url)
  for (const file of readdirSync(dir).filter((name) => name.endsWith('.json'))) {
    const config = JSON.parse(readFileSync(new URL(file, dir), 'utf8')).config
    for (const key of ['title', 'no_endpoint', 'none_active', 'no_default', 'unknown', 'how', 'waiting']) {
      assert.ok(typeof config[`audio_no_default_${key}`] === 'string' && config[`audio_no_default_${key}`].length > 0, `${file}: audio_no_default_${key}`)
    }
    assert.match(config.audio_no_default_how, /VB-CABLE/, file)
    assert.match(config.audio_no_default_how, /Steam Streaming Speakers/, file)
  }
})

test('the notice on the audio page says why there is no sound, what to do, and offers the two virtual speakers', async () => {
  const state = normalizeAudioDevices(NO_DEVICE)
  const shown = await h.mount(NoDefaultOutputNotice, { state, loaded: true })
  assert.match(shown.text(), /config\.audio_no_default_title/)
  assert.match(shown.text(), /config\.audio_no_default_no_endpoint/)
  assert.match(shown.text(), /config\.audio_no_default_how/)
  assert.match(shown.text(), /config\.audio_no_default_waiting/, 'a running stream that waits for a device says so')
  assert.equal(shown.buttons('VB-CABLE').length, 1)
  assert.equal(shown.buttons('Steam Streaming Speakers').length, 1)
  shown.unmount()
  // the reason decides the sentence; a stream that is not running says nothing about waiting
  const disabledOnly = await h.mount(NoDefaultOutputNotice, { state: normalizeAudioDevices({ ...NO_DEVICE, default_output: { present: false, reason: 'none_active', host_waiting: false } }), loaded: true })
  assert.match(disabledOnly.text(), /config\.audio_no_default_none_active/)
  assert.doesNotMatch(disabledOnly.text(), /config\.audio_no_default_waiting/)
  disabledOnly.unmount()
  // nothing is shown while the list loads, after a failed read, or when a device is the default
  for (const props of [{ state, loaded: false }, { state, loaded: true, failed: true }, { state: normalizeAudioDevices(ANSWER), loaded: true }]) {
    const quiet = await h.mount(NoDefaultOutputNotice, props)
    assert.doesNotMatch(quiet.text(), /audio_no_default/)
    quiet.unmount()
  }
})
