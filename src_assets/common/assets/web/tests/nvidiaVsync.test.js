import test from 'node:test'
import assert from 'node:assert/strict'
import { nvidiaVsyncUi } from '../utils/nvidiaVsync.js'

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
