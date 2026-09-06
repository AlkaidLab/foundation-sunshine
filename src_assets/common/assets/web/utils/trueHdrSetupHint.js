// Actionable guidance when the TrueHDR backend is missing or failed to load.
// Technical strings stay in English on purpose: they reference file names and
// build commands verbatim, matching the rest of the diagnostics panel.
export function getTrueHdrSetupHint(pipeline) {
  if (!pipeline || pipeline.hdr_mode === 'sdr') return ''
  if (pipeline.synthetic_hdr_state !== 'degraded') return ''

  const reason = pipeline.synthetic_hdr_failure_reason || ''
  if (!reason.includes('backend')) return ''

  // Load/ABI failures mean the DLL is missing, misconfigured, or the wrong
  // build. Keep the guidance deterministic: a runtime discovered inside a
  // vendor installation is not necessarily compatible or safe to relocate.
  const isLoadFailure =
    reason === 'backend_path_not_absolute' ||
    reason.startsWith('backend_load_failed') ||
    reason === 'backend_export_missing' ||
    reason === 'backend_abi_mismatch' ||
    reason === 'backend_api_incomplete'
  if (isLoadFailure) {
    return "Build foundation_truehdr_backend.dll with scripts/build-rtx-hdr-backend.ps1 using the NVIDIA RTX Video SDK, keep the user-provided nvngx_truehdr.dll beside it, then set the backend DLL's absolute path in the Advanced tab."
  }

  // Everything else under the "backend" prefix is a runtime failure: the DLL
  // loaded but NGX failed (device lost, runtime unavailable, invalid argument,
  // ...). Rebuilding the backend will not help.
  return `TrueHDR backend failed at runtime (${reason}). This is a GPU/driver/SDK issue — verify NVIDIA App (RTX Video) and the GPU driver, then restart the stream.`
}
