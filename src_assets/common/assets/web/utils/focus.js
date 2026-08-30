export const FOCUSABLE_SELECTOR = [
  'button:not([disabled])',
  'a[href]',
  'input:not([disabled])',
  'select:not([disabled])',
  'textarea:not([disabled])',
  '[tabindex]:not([tabindex="-1"])',
].join(', ')

/**
 * Collect focusable descendants that are available to a dialog's keyboard trap.
 *
 * @param {Element | null | undefined} container Dialog or overlay container.
 * @returns {Element[]} Visible focusable descendants in document order.
 */
export function getFocusableElements(container) {
  if (!container) return []

  return Array.from(container.querySelectorAll(FOCUSABLE_SELECTOR)).filter((element) => {
    if (element.hasAttribute('hidden') || element.getAttribute('aria-hidden') === 'true') return false

    const style = window.getComputedStyle(element)
    return style.display !== 'none' && style.visibility !== 'hidden'
  })
}

/**
 * Keep nested overlays inside the closest Bootstrap modal so its focus trap accepts them.
 *
 * @param {Element | null | undefined} element Element that opened the overlay.
 * @param {string | Element} fallback Teleport target when the overlay is not nested in a modal.
 * @returns {string | Element} A valid Vue Teleport target.
 */
export function resolveDialogTeleportTarget(element, fallback = 'body') {
  if (!element || typeof element.closest !== 'function') return fallback
  return element.closest('.modal') || fallback
}
