export const DEFAULT_EDID_RESOLUTION = 'default'

function parseSize(value) {
  const match = /^(\d+)[xX](\d+)$/.exec(String(value).trim())
  if (!match) return null
  const width = Number(match[1]), height = Number(match[2])
  if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1 || width > 4095 || height > 4095) return null
  return { width, height, value: `${width}x${height}` }
}

function encodableAt(size, value) {
  if ((typeof value !== 'number' && typeof value !== 'string') || !/^\d+(\.\d+)?$/.test(String(value))) return false
  const fps = Number(value)
  if (!String(value).trim() || !Number.isFinite(fps) || fps < 1 || fps > 480) return false
  const rate = Math.round(fps)
  const clock = rate === 60 && size.width === 1920 && size.height === 1080 ? 14850 :
    rate === 60 && size.width === 3840 && size.height === 2160 ? 59400 :
    Math.round((size.width + 4) * (size.height + 4) * rate / 10000)
  return clock > 0 && clock <= 65535
}

export function edidResolutionOptions(resolutions = [], fps = [], selected = DEFAULT_EDID_RESOLUTION) {
  const options = [{ value: DEFAULT_EDID_RESOLUTION, reason: '' }]
  // Match useConfig's existing FPS serializer: rates outside 30..500 are
  // removed before the API request, so they cannot make a choice available.
  const persistedFps = fps.filter(value => Number(value) >= 30 && Number(value) <= 500)
  for (const entry of resolutions) {
    const size = parseSize(entry)
    const value = size?.value ?? String(entry)
    if (options.some(option => option.value === value)) continue
    options.push({ value, reason: size && persistedFps.some(rate => encodableAt(size, rate)) ? '' : 'not_encodable' })
  }
  if (!options.some(option => option.value === selected)) {
    options.push({ value: selected, reason: 'not_advertised' })
  }
  return options
}

export function validEdidResolutionSelection(selected, resolutions, fps) {
  return edidResolutionOptions(resolutions, fps, selected).some(option => option.value === selected && !option.reason)
}
