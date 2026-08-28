const DISPLAY_PROFILE_FIELDS = new Set([
  'display-target',
  'display-device-prep',
  'display-resolution-mode',
  'display-refresh-rate-mode',
  'display-output-name',
  'display-resolution',
  'display-refresh-rate',
  'display-hdr',
])

/**
 * Normalize the per-app display scheme fields before saving.
 *
 * Apps without a display target keep no display fields at all, so the client
 * request and the global configuration stay fully in control. Apps with a
 * target only keep valid values; fixed resolution / refresh rate values are
 * dropped unless the matching mode can apply them, and the fixed HDR state
 * must be one of the supported values.
 */
export function normalizeAppDisplayProfile(app) {
  const normalized = { ...app }

  for (const key of Object.keys(normalized)) {
    if (key.startsWith('display-') && !DISPLAY_PROFILE_FIELDS.has(key)) delete normalized[key]
  }

  if (!['physical', 'virtual'].includes(normalized['display-target'])) {
    for (const key of DISPLAY_PROFILE_FIELDS) delete normalized[key]
    return normalized
  }

  if (!['', 'no_operation', 'client'].includes(normalized['display-resolution-mode'])) {
    normalized['display-resolution-mode'] = ''
  }
  // The refresh rate has no per-app "ignore client" gate in the 0-change
  // design (upstream applies the client fps unconditionally in the
  // automatic branch), so `no_operation` would silently do nothing while
  // also disabling the shared sops gate for the resolution. Drop it.
  if (!['', 'client'].includes(normalized['display-refresh-rate-mode'])) {
    normalized['display-refresh-rate-mode'] = ''
  }
  if (normalized['display-resolution'] && !/^[1-9]\d{1,4}x[1-9]\d{1,4}$/.test(normalized['display-resolution'])) {
    normalized['display-resolution'] = ''
  }
  if (normalized['display-refresh-rate'] && !/^[1-9]\d{0,3}(?:\.\d+)?$/.test(normalized['display-refresh-rate'])) {
    normalized['display-refresh-rate'] = ''
  }
  if (!['', 'on', 'off'].includes(normalized['display-hdr'])) {
    normalized['display-hdr'] = ''
  }
  if (normalized['display-target'] !== 'physical') delete normalized['display-output-name']
  return normalized
}
