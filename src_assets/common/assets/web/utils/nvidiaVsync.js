const enabled = value => ['enabled', 'true', '1', 'yes', 'on', 'enable'].includes(String(value).toLowerCase())

export function nvidiaVsyncUi(config, platform = 'windows') {
  const checked = enabled(config.nvenc_vdd_force_vsync)
  const capability = config.nvidia_vsync
  const supported = enabled(capability?.supported)
  const managed = config.nvenc_manage_driver_settings == null || enabled(config.nvenc_manage_driver_settings)
  const state = !supported ? 'unsupported' : !managed ? 'unmanaged' :
    capability?.state === 'active' ? 'active' : capability?.state === 'error' ? 'error' :
    checked ? 'next_launch' : 'disabled'
  return {
    visible: platform === 'windows' && (supported || checked),
    disabled: !supported || !managed,
    checked,
    state,
  }
}
