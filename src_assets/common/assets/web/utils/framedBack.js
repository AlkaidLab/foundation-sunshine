// Inside the control panel's frame the page draws its own Back in its title row (MPage's arrow), the way the Web UI does on a phone. The panel says when Back applies (`foundation-panel-back`), the page
// says whether a page of it can draw it (`foundation-back-inline`: until then, or from an older Web, the panel keeps a bar of its own) and a press goes to the panel (`foundation-back-click`), which goes back
// exactly where it went before. The object this makes is the `m-scene` the pages of m3e inject: canBack, back, and a page announcing itself.
import { ref } from 'vue'

export const BACK_REPORT = 'foundation-back-inline'
export const BACK_CLICK = 'foundation-back-click'
export const BACK_REQUEST = 'foundation-back-request'
export const PANEL_BACK = 'foundation-panel-back'
/** how long a frame without any page stays unreported: a swap of the view unmounts one page and mounts the next in the same moment, and the loading state of a chunk is not an answer */
export const BACK_SETTLE_MS = 200

/** `post(message)` sends to the panel (the parent window); `parent` is that window, the only sender listened to. */
export function createFramedBack({ post, parent, setTimer = setTimeout, clearTimer = clearTimeout, settleMs = BACK_SETTLE_MS }) {
  const show = ref(false)
  let pages = 0
  let reported = null
  let timer = 0
  const report = (drawn) => { if (reported === drawn) return; reported = drawn; post({ type: BACK_REPORT, drawn }) }
  const settle = () => {
    clearTimer(timer)
    if (pages > 0) report(true)
    else timer = setTimer(() => { if (pages <= 0) report(false) }, settleMs)
  }
  const scene = {
    canBack: show,
    back: () => post({ type: BACK_CLICK }),
    pageMounted: () => { pages += 1; settle() },
    pageUnmounted: () => { pages = Math.max(0, pages - 1); settle() },
  }
  const onMessage = (event) => {
    if (event.source !== parent) return
    const data = event.data
    if (data?.type === PANEL_BACK && typeof data.show === 'boolean') show.value = data.show
  }
  /** after the frame has mounted: ask for the state of Back, and give the panel a definite answer about this document */
  const start = () => { post({ type: BACK_REQUEST }); settle() }
  return { scene, onMessage, start, show }
}
