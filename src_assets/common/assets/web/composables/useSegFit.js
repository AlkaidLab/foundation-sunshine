import { getCurrentInstance, onBeforeUnmount, onMounted, onUpdated, ref } from 'vue'

// The one rule for how a choice is drawn on the pages of the settings: a connected button group when every name is read whole, one press away; a value row that opens the list
// when the names do not fit the width the row really has. The width is measured (the row of the page, whatever the window or the pane), not guessed from the window: a phone, a narrow
// pane of a desktop window and a large text size all end the same way, with nothing cut off and nothing out of reach.

// a letter of the button label (500 14px) is about 8 px, a Chinese character 14 px
export const labelPx = (text) => [...String(text)].reduce((sum, ch) => sum + (ch.charCodeAt(0) > 0x2e80 ? 14 : 8), 0)

/** What a group of equal buttons needs: the widest name and 16 px of padding either side, a button is a finger wide at least, a gap of 4 px between. */
export const segNeed = (labels, { pad = 16, gap = 4, hit = 44 } = {}) => {
  if (!labels.length) return 0
  const widest = Math.max(...labels.map((label) => Math.ceil(labelPx(label))))
  return labels.length * Math.max(hit, widest + 2 * pad) + (labels.length - 1) * gap
}

/** `width` 0 is 'not measured yet' (before the first layout, a test): the group is kept. */
export const segFits = (labels, width, options) => !(width > 0) || segNeed(labels, options) <= width

/** The width of what the component draws (its own root element), kept up to date. `chrome` is what the row takes besides the control (its padding, the indent of an icon). */
export function useRowWidth(chrome = () => 32) {
  const instance = getCurrentInstance()
  const width = ref(0)
  let el = null
  let observer = null
  const measure = () => { const w = el ? el.getBoundingClientRect().width : 0; width.value = w > 0 ? Math.max(0, Math.floor(w - chrome())) : 0 }
  const attach = () => {
    const next = instance?.proxy?.$el
    if (!next || next === el || next.nodeType !== 1) return
    observer?.disconnect()
    el = next
    measure()
    if (typeof ResizeObserver === 'function') { observer = new ResizeObserver(measure); observer.observe(el) }
  }
  onMounted(attach)
  onUpdated(attach)
  onBeforeUnmount(() => { observer?.disconnect(); observer = null; el = null })
  return width
}
