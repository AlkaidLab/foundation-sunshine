import test from 'node:test'
import assert from 'node:assert/strict'
import { nvidiaVsyncUi, observeNvidiaVsyncStatus } from '../utils/nvidiaVsync.js'

test('NVIDIA capability comes from the host, independent of encoder or OSD', () => {
  assert.equal(nvidiaVsyncUi({ nvidia_vsync: { supported: 'true' }, encoder: 'amdvce' }).visible, true)
  assert.equal(nvidiaVsyncUi({ encoder: 'nvenc' }).visible, false)
  assert.equal(nvidiaVsyncUi({ nvidia_vsync: { supported: true } }, 'linux').visible, false)
})
test('unsupported hosts retain an enabled preference without permitting changes', () => {
  const view = nvidiaVsyncUi({ nvenc_vdd_force_vsync: 'enabled', nvidia_vsync: { supported: false } })
  assert.deepEqual(view, { visible: true, checked: true, disabled: true, state: 'unsupported' })
})
test('management opt-out disables the option, while active status never comes from the checkbox', () => {
  const config = { nvenc_vdd_force_vsync: true, nvidia_vsync: { supported: true } }
  assert.equal(nvidiaVsyncUi(config).state, 'next_launch')
  assert.equal(nvidiaVsyncUi({ ...config, nvenc_manage_driver_settings: false }).state, 'unmanaged')
  assert.equal(nvidiaVsyncUi({ ...config, nvidia_vsync: { supported: true, state: 'active' } }).state, 'active')
  assert.equal(nvidiaVsyncUi({ ...config, nvidia_vsync: { supported: true, state: 'error' } }).state, 'error')
})

test('capability errors and absent reports are distinct from confirmed unsupported hardware', () => {
  const view = nvidiaVsyncUi({ nvenc_vdd_force_vsync: 'enabled', nvidia_vsync: { supported: false, detection_known: false } })
  assert.equal(view.state, 'unknown')
  assert.equal(view.visible, true)
  assert.equal(view.disabled, true)
  assert.equal(nvidiaVsyncUi({ nvenc_vdd_force_vsync: true }).state, 'unknown')
  assert.equal(nvidiaVsyncUi({ nvidia_vsync: { supported: false, detection_known: true } }).state, 'unsupported')
})

const tick = () => new Promise(resolve => setImmediate(resolve))
function statusEvents() {
  const windowTarget = new EventTarget()
  const documentTarget = new EventTarget()
  documentTarget.visibilityState = 'visible'
  return { windowTarget, documentTarget }
}

test('focus and visibility refresh only host status and coalesce concurrent requests', async () => {
  const events = statusEvents()
  let complete
  let requests = 0
  const statuses = []
  const stop = observeNvidiaVsyncStatus({ ...events,
    load: () => { requests++; return new Promise(resolve => { complete = resolve }) },
    onStatus: data => statuses.push(data.nvidia_vsync.state), onError: assert.fail,
  })
  events.windowTarget.dispatchEvent(new Event('focus'))
  events.documentTarget.dispatchEvent(new Event('visibilitychange'))
  assert.equal(requests, 1)
  complete({ status: true, nvidia_vsync: { state: 'active' } })
  await tick()
  events.documentTarget.visibilityState = 'hidden'
  events.windowTarget.dispatchEvent(new Event('focus'))
  assert.equal(requests, 1)
  events.documentTarget.visibilityState = 'visible'
  events.documentTarget.dispatchEvent(new Event('visibilitychange'))
  complete({ status: 'true', nvidia_vsync: { state: 'idle' } })
  await tick()
  assert.deepEqual(statuses, ['active', 'idle'])
  stop()
  events.windowTarget.dispatchEvent(new Event('focus'))
  assert.equal(requests, 2)
})

test('unmount aborts an outstanding refresh and prevents stale status updates', async () => {
  const events = statusEvents()
  let complete, signal
  let changes = 0
  const stop = observeNvidiaVsyncStatus({ ...events,
    load: options => { signal = options.signal; return new Promise(resolve => { complete = resolve }) },
    onStatus: () => changes++, onError: () => changes++,
  })
  events.windowTarget.dispatchEvent(new Event('focus'))
  stop()
  assert.equal(signal.aborted, true)
  complete({ status: true })
  await tick()
  assert.equal(changes, 0)
})

test('failed host responses report a query failure and allow a later retry', async () => {
  const events = statusEvents()
  let errors = 0
  let statuses = 0
  const stop = observeNvidiaVsyncStatus({ ...events,
    load: async () => ({ status: false }), onStatus: () => statuses++, onError: () => errors++,
  })
  events.windowTarget.dispatchEvent(new Event('focus'))
  await tick()
  events.windowTarget.dispatchEvent(new Event('focus'))
  await tick()
  assert.equal(errors, 2)
  assert.equal(statuses, 0)
  stop()
})

test('active overrides and restoration failures stay visible after management opt-out', () => {
  for (const state of ['active', 'error']) {
    const view = nvidiaVsyncUi({
      nvenc_manage_driver_settings: 'disabled', nvenc_vdd_force_vsync: 'disabled',
      nvidia_vsync: { supported: false, detection_known: false, state },
    })
    assert.equal(view.state, state)
    assert.equal(view.visible, true)
    assert.equal(view.disabled, true)
  }
})
