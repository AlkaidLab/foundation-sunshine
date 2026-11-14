// Inside the control panel's frame: wear what the panel says about its root element (see the panel's frameRoot.js). Only the panel (the parent
// window) is listened to, and only what looks like a colour token is applied: no picture, no script, no other property.
const PROP = /^--(md-sys-color|md-ref|m-)[a-z0-9-]*$/
const VALUE = /^[#a-z0-9(),.%\s/+*-]{1,200}$/i
const BAD = /url\(|expression|;|\{|\}|\\/i
const CLASSES = ['m-has-wallpaper', 'dark']
// the text of the panel's theme style element: only the selectors and the colour declarations the theme writes, line by line
const THEME_LINE = /^(?:\}|(?::root, )?\[data-(?:bs-)?theme='(?:light|dark)'\](?:, \[data-theme='(?:light|dark)'\])*\s*\{|--[a-z0-9-]+:\s*[#a-z0-9(),.%\s/+*-]+;)$/i
export const themeCssOk = (css) => typeof css === 'string' && css.length < 60000 && css.split('\n').every((line) => line.trim() === '' || THEME_LINE.test(line.trim()))

/** Applies one message to a root element; returns the names it set (to be removed when the next message no longer has them). */
export function applyRootMessage(root, data, previous = new Set()) {
  if (!data || data.type !== 'foundation-root' || typeof data.props !== 'object' || !data.props) return previous
  const set = new Set()
  for (const [name, value] of Object.entries(data.props)) {
    if (!PROP.test(name) || typeof value !== 'string' || !VALUE.test(value) || BAD.test(value)) continue
    root.style.setProperty(name, value); set.add(name)
  }
  for (const name of previous) if (!set.has(name)) root.style.removeProperty(name)
  const classes = Array.isArray(data.classes) ? data.classes : []
  for (const c of CLASSES) root.classList.toggle(c, classes.includes(c))
  if (data.theme === 'light' || data.theme === 'dark') root.setAttribute('data-bs-theme', data.theme)
  // the list pane (个性化): the page in the frame draws its list the way the panel does
  if (data.list === 'merged' || data.list === 'one') root.setAttribute('data-list', data.list)
  if (data.themeCss && themeCssOk(data.themeCss) && root.ownerDocument) {
    const doc = root.ownerDocument
    let style = doc.getElementById('foundation-ui-theme')
    if (!style) { style = doc.createElement('style'); style.id = 'foundation-ui-theme'; doc.head.appendChild(style) }
    style.textContent = data.themeCss
  }
  return set
}

export function startEmbeddedRoot(root = document.documentElement) {
  let names = new Set()
  window.addEventListener('message', (event) => {
    if (event.source !== window.parent) return
    names = applyRootMessage(root, event.data, names)
  })
  window.parent.postMessage({ type: 'foundation-root-request' }, '*')
}
