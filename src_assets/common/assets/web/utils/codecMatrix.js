// The rows of the codec matrix: what is offered to clients for each codec, with 10-bit (HDR), and whether the stream in
// progress uses it. `active_hevc_mode` and `active_av1_mode` are what the host's encoder probe resolved the two settings to
// (1 not offered, 2 offered, 3 offered with 10-bit; 0 or absent: not probed, or a host that does not report them). Without a
// probe result the configured value is read instead (0 automatic, 1 never, 2 offered, 3 offered with 10-bit), and
// automatic shows as "auto": the host decides from what the encoder reports.
const state = (mode, configured) => {
  const m = Number(mode)
  if (m >= 1) return { offered: m === 1 ? 'no' : 'yes', deep: m === 3 ? 'yes' : 'no', probed: true }
  const c = Number(configured ?? 0)
  return { offered: c === 1 ? 'no' : c === 0 ? 'auto' : 'yes', deep: c === 3 ? 'yes' : c === 0 ? 'auto' : 'no', probed: false }
}

export function codecRows(config = {}, sessionCodec = '') {
  const now = String(sessionCodec || '').toLowerCase().replace(/[.\-]/g, '')
  return [
    { id: 'h264', name: 'H.264', offered: 'yes', deep: 'no', probed: true },
    { id: 'hevc', name: 'HEVC', ...state(config.active_hevc_mode, config.hevc_mode) },
    { id: 'av1', name: 'AV1', ...state(config.active_av1_mode, config.av1_mode) },
  ].map((row) => ({ ...row, now: now === row.id }))
}
