/**
 * Warning shown before the DualSense advanced options are revealed. The
 * "do not show again" choice is stored per browser in localStorage.
 */
export const ADVANCED_WARNING_HIDDEN_KEY = 'ds5.advanced.warning.hidden'
export const ADVANCED_WARNING_SECONDS = 5

/**
 * @param {Pick<Storage, 'getItem'> | null | undefined} storage
 * @returns {boolean} True when the user chose not to see the warning again.
 */
export function isAdvancedWarningHidden(storage) {
  try {
    return storage?.getItem(ADVANCED_WARNING_HIDDEN_KEY) === '1'
  }
  catch {
    // A locked-down browser can refuse storage access; the warning then always shows.
    return false
  }
}

/**
 * @param {Pick<Storage, 'setItem' | 'removeItem'> | null | undefined} storage
 * @param {boolean} hidden
 */
export function setAdvancedWarningHidden(storage, hidden) {
  try {
    if (hidden) storage?.setItem(ADVANCED_WARNING_HIDDEN_KEY, '1')
    else storage?.removeItem(ADVANCED_WARNING_HIDDEN_KEY)
  }
  catch {
    // Without storage the choice only lasts for this page load.
  }
}

/**
 * Whole seconds left on the confirm countdown, rounded up so the button shows
 * the full count at first and unlocks exactly when the time is over.
 *
 * @param {number} elapsedMs Milliseconds since the dialog opened.
 * @param {number} [total] Countdown length in seconds.
 * @returns {number}
 */
export function remainingSeconds(elapsedMs, total = ADVANCED_WARNING_SECONDS) {
  const left = total - Math.max(0, elapsedMs) / 1000
  return Math.max(0, Math.ceil(left))
}
