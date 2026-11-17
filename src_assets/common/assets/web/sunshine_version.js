// The date part of a Foundation Sunshine version, the same rules as src/version_compare.h (host) and update/metadata.rs (panel); the three share
// tests/fixtures/version_vectors.json. YYYY.MDD.HMMSS, padded or not; what follows (.杂鱼, .<commit>, emoji, anything) never counts;
// manual-YYYYMMDD-HHMMSS[-dev] compares by its date; a string with no date version is unknown (never newer, older or equal).
const MAX_DIGITS = 9

const readNumber = (text, pos, wantDigits = 0) => {
  let n = 0
  while (pos + n < text.length && text.charCodeAt(pos + n) >= 48 && text.charCodeAt(pos + n) <= 57) {
    if (n >= MAX_DIGITS) return null
    n++
  }
  if (n === 0 || (wantDigits && n !== wantDigits)) return null
  return { value: Number(text.slice(pos, pos + n)), next: pos + n }
}

/** The first three parts of `version` as numbers, or null when it has no date version. */
export function versionKey(version) {
  if (typeof version !== 'string') return null
  let text = version.replace(/^[ \t\r\n]+/, '')
  if (text[0] === 'v' || text[0] === 'V') text = text.slice(1)

  if (text.slice(0, 7).toLowerCase() === 'manual-') {
    const date = readNumber(text, 7, 8)
    if (!date || text[date.next] !== '-') return null
    const time = readNumber(text, date.next + 1, 6)
    if (!time) return null
    const month = Math.floor(date.value / 100) % 100
    const day = date.value % 100
    if (month < 1 || month > 12 || day < 1 || day > 31) return null
    return [Math.floor(date.value / 10000), date.value % 10000, time.value]
  }

  const key = []
  let pos = 0
  for (let i = 0; i < 3; i++) {
    const part = readNumber(text, pos)
    if (!part) return null
    key.push(part.value)
    pos = part.next
    if (i < 2) {
      if (text[pos] !== '.') return null
      pos++
    }
  }
  return key
}

/** -1, 0, 1 as `left` is older than, the same as, newer than `right`; null when either has no date version. */
export function compareVersions(left, right) {
  const a = versionKey(left)
  const b = versionKey(right)
  if (!a || !b) return null
  for (let i = 0; i < 3; i++) {
    if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1
  }
  return 0
}

class SunshineVersion {
  constructor(release = null, version = null) {
    if (release) {
      this.release = release
      this.version = release.tag_name
      this.versionName = release.name
      this.versionTag = release.tag_tag
    } else if (version) {
      this.release = null
      this.version = version
      this.versionName = null
      this.versionTag = null
    } else {
      throw new Error('Either release or version must be provided')
    }
    this.versionParts = this.parseVersion(this.version)
    this.versionMajor = this.versionParts ? this.versionParts[0] : null
    this.versionMinor = this.versionParts ? this.versionParts[1] : null
    this.versionPatch = this.versionParts ? this.versionParts[2] : null
  }

  parseVersion(version) {
    return versionKey(version)
  }

  /** -1, 0, 1 as this version is older than, the same as, newer than `otherVersion`; null when either has no date version. */
  compareTo(otherVersion) {
    if (otherVersion instanceof SunshineVersion) return compareVersions(this.version, otherVersion.version)
    if (typeof otherVersion === 'string') return compareVersions(this.version, otherVersion)
    throw new Error('Invalid argument: otherVersion must be a SunshineVersion object or a version string')
  }

  isGreater(otherVersion) {
    return this.compareTo(otherVersion) === 1
  }

  isLessThan(otherVersion) {
    return this.compareTo(otherVersion) === -1
  }

  isEqual(otherVersion) {
    return this.compareTo(otherVersion) === 0
  }
}

export default SunshineVersion
