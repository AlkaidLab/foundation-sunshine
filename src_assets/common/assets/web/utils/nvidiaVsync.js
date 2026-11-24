const enabled = value => ['enabled', 'true', '1', 'yes', 'on', 'enable'].includes(String(value).trim().toLowerCase())

/** Derive presentation from host capability without treating a draft checkbox as applied. */
export function nvidiaVsyncUi(config, platform = 'windows') {
  const checked = enabled(config.nvenc_vdd_force_vsync)
  const capability = config.nvidia_vsync
  const supported = enabled(capability?.supported)
  // Hosts predating detection_known still provide an explicit supported field.
  const known = capability?.detection_known == null ? capability?.supported != null : enabled(capability.detection_known)
  const managed = config.nvenc_manage_driver_settings == null || enabled(config.nvenc_manage_driver_settings)
  const lifecycle = ['active', 'error'].includes(capability?.state)
  const state = lifecycle ? capability.state : !known ? 'unknown' : !supported ? 'unsupported' :
    !managed ? 'unmanaged' : checked ? 'next_launch' : 'disabled'
  return {
    visible: platform === 'windows' && (lifecycle || supported || checked || (capability != null && !known)),
    disabled: !known || !supported || !managed,
    checked,
    state,
  }
}

/** Refresh read-only host status on focus/visibility; dispose aborts requests and removes listeners. */
export function observeNvidiaVsyncStatus({ load, onStatus, onError,
  windowTarget = globalThis.window, documentTarget = globalThis.document }) {
  let disposed = false
  let controller = null
  let timeout
  const refresh = async () => {
    if (disposed || controller || documentTarget.visibilityState === 'hidden') return
    controller = new AbortController()
    timeout = setTimeout(() => controller?.abort(), 5000)
    try {
      const data = await load({ signal: controller.signal })
      if (disposed) return
      if (String(data?.status) !== 'true') throw new Error('Invalid host status response')
      onStatus(data)
    } catch (error) {
      if (!disposed) onError(error)
    } finally {
      clearTimeout(timeout)
      controller = null
    }
  }
  windowTarget.addEventListener('focus', refresh)
  documentTarget.addEventListener('visibilitychange', refresh)
  return () => {
    disposed = true
    windowTarget.removeEventListener('focus', refresh)
    documentTarget.removeEventListener('visibilitychange', refresh)
    clearTimeout(timeout)
    controller?.abort()
  }
}
