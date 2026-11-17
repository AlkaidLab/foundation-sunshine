// Icon set inside the thick sliders: one glyph per kind of setting, picked from the key name.
const RULES = [
  [/\.audio\.micLevel$/, 'mic'],
  [/\.audio\./, 'volume_up'],
  [/\.led\./, 'lightbulb'],
  [/\.trigger\.|\.axis\.trigger\./, 'touch_app'],
  [/\.haptics\.|\.rumble\./, 'mobile_vibrate'],
  [/\.gyro\./, 'sensors'],
  [/\.touchpad\./, 'touch_app'],
  [/\.stick\./, 'gamepad'],
  [/\.sync\.|\.compat\./, 'sync'],
]

/** @param {string} key Tuning key name, for example ds5.haptics.hd.gain. */
export function sliderIcon(key) {
  const hit = RULES.find(([pattern]) => pattern.test(`.${key}`))
  return hit ? hit[1] : ''
}

// Pairs of keys that are the two ends of one range (dead zones, travel): the page shows one range slider for both.
const PAIRS = [
  ['deadzoneInner', 'deadzoneOuter'],
  ['rangeMin', 'rangeMax'],
]

/**
 * Turns a list of key descriptors into rows: a pair whose two ends are both in the list becomes one range row.
 * @param {Array<{key: string}>} keys
 * @returns {Array<{ pair: boolean, key: object, high?: object }>}
 */
export function rowsOf(keys) {
  const byName = new Map(keys.map(key => [key.key, key]))
  const taken = new Set()
  const rows = []
  for (const key of keys) {
    if (taken.has(key.key)) continue
    const pair = PAIRS.find(([low]) => key.key.endsWith(`.${low}`))
    const high = pair && byName.get(key.key.slice(0, -pair[0].length) + pair[1])
    if (high && !taken.has(high.key)) {
      taken.add(high.key)
      rows.push({ pair: true, key, high })
    }
    else if (!pair || !byName.has(key.key.slice(0, -pair[0].length) + pair[1])) {
      rows.push({ pair: false, key })
    }
  }
  return rows
}

/** Splits rows into blocks of one family of keys (same name up to the last part), so a long group reads in short cards. */
export function blocksOf(rows) {
  const blocks = []
  let last = null
  for (const row of rows) {
    const family = row.key.key.split('.').slice(0, -1).join('.')
    if (family !== last) blocks.push([])
    last = family
    blocks[blocks.length - 1].push(row)
  }
  return blocks
}

// The two ends of a slider, [less, more], picked from the kind of the setting. Every single-handle slider has a pair; the centred offsets (an offset around zero) and the two-handle ranges are drawn
// by the shared slider without inside icons (the track is anchored in the middle or has two handles), so they have none.
const ENDS = [
  [/\.audio\.micLevel$/, ['mic_off', 'mic']],
  [/\.audio\.(speaker|headset)Level$/, ['volume_down', 'volume_up']],
  [/\.led\.lightbar\.brightness$/, ['dark_mode', 'light_mode']],
  [/\.(haptics|rumble)\.[A-Za-z.]*(gain|strength|intensity|sharpness|curve|lowGain|highGain|lowBandDb|highBandDb)$/i, ['mobile_off', 'mobile_vibrate']],
  [/(latencyMs|smoothingMs|holdMs|sendIntervalMs|watchdogMs|maxDurationMs)$/, ['hourglass_empty', 'hourglass_bottom']],
  [/(crossoverHz|filterHz|maxRateHz|reportRateCapHz)$/, ['slow_motion_video', 'speed']],
  [/\.(sensitivity|scrollSpeed)$/, ['mobile_sensor_lo', 'mobile_sensor_hi']],
]

/** @param {string} key Tuning key name. @returns {[string, string]} the icon at the start and at the end of its slider */
export function sliderEnds(key) {
  const hit = ENDS.find(([pattern]) => pattern.test(`.${key}`))
  return hit ? hit[1] : ['remove', 'add']
}
