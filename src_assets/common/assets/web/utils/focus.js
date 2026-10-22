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
 * @param {string} selector Focusable controls, optionally including native disclosure controls.
 * @returns {Element[]} Visible focusable descendants in document order.
 */
export function getFocusableElements(container, selector = FOCUSABLE_SELECTOR) {
  if (!container) return []

  return Array.from(container.querySelectorAll(selector)).filter((element) => {
    if (element.hasAttribute('hidden') || element.getAttribute('aria-hidden') === 'true') return false

    const style = window.getComputedStyle(element)
    return style.display !== 'none' && style.visibility !== 'hidden'
  })
}

// Move one step through a dialog's available controls, wrapping at either end.
export function cycleDialogFocus(event, container, focusable) {
  event.preventDefault()
  if (!focusable.length) {
    container?.focus()
    return
  }
  const index = focusable.indexOf(document.activeElement)
  const next = event.shiftKey ? (index <= 0 ? focusable.length - 1 : index - 1) : (index + 1) % focusable.length
  focusable[next].focus()
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
