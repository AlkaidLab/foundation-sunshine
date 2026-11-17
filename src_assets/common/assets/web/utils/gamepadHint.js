// The one-line hint for a client whose PlayStation pad runs as a DualShock 4: which reason applies and the key of the
// line to show. The host's session fields decide (gamepad_type, gamepad_dualsense_identity of /api/runtime/sessions);
// the caller localizes the key:
//   host    the client's arrival identified a DualSense and this host's own type selection is DS4
//   client  the client, a per-app setting or a DualSense identity profile chose DS4
//   legacy  the client did not report which PlayStation pad it holds (older clients never do)
export function ds4HintReason(session, hostMode = '') {
  if (!session || session.gamepad_type !== 'ds4') return ''
  if (session.gamepad_dualsense_identity !== true) return 'legacy'
  return hostMode === 'ds4' ? 'host' : 'client'
}

/** The first session that runs a PlayStation pad as a DS4, as { reason, key } the caller can translate; null without one. */
export function ds4HintOf(sessions, hostMode = '') {
  const list = Array.isArray(sessions) ? sessions : []
  const session = list.find((s) => s && s.gamepad_type === 'ds4')
  const reason = ds4HintReason(session, hostMode)
  return reason ? { reason, key: `config.gamepad_ds4_hint_${reason}` } : null
}
