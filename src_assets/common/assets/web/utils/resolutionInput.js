export const RESOLUTION_INPUT_PATTERN = String.raw`\s*[0-9]+\s*[xX*×]\s*[0-9]+\s*`

export function normalizeResolution(value) {
  const match = typeof value === 'string'
    ? value.match(/^\s*(\d+)\s*[xX*×]\s*(\d+)\s*$/)
    : null
  return match ? `${match[1]}x${match[2]}` : ''
}
