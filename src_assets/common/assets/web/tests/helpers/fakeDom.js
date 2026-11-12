// A small DOM for tests that mount a real page with Vue's DOM runtime (no browser, no jsdom in this repository): elements keep their attributes, children, text and event listeners,
// which is what the pages and foundation-ui read and write. Import this file BEFORE anything that loads Vue: Vue's runtime reads `document` once, when it is loaded.
// Only what the pages need is here; anything else a page asks of the DOM is a no-op, so a test that needs more says so by failing.
export class FakeNode {
  constructor(tag, type = 1) {
    this.nodeType = type
    this.tagName = tag
    this.nodeName = tag?.toUpperCase()
    this.childNodes = []
    this.parentNode = null
    this.attrs = {}
    this.listeners = {}
    this.dataset = {}
    this._text = ''
    this.style = new Proxy({}, { get: (t, k) => (k === 'setProperty' || k === 'removeProperty' ? () => {} : t[k] ?? ''), set: (t, k, v) => { t[k] = v; return true } })
  }

  get firstChild() { return this.childNodes[0] || null }
  get lastChild() { return this.childNodes[this.childNodes.length - 1] || null }
  get nextSibling() { const p = this.parentNode; return p ? p.childNodes[p.childNodes.indexOf(this) + 1] || null : null }
  get previousSibling() { const p = this.parentNode; return p ? p.childNodes[p.childNodes.indexOf(this) - 1] || null : null }
  get children() { return this.childNodes.filter((c) => c.nodeType === 1) }
  get isConnected() { return true }
  get nodeValue() { return this._text }
  set nodeValue(v) { this._text = String(v) }
  get data() { return this._text }
  set data(v) { this._text = String(v) }
  get textContent() { return this.nodeType === 3 || this.nodeType === 8 ? this._text : this.childNodes.filter((c) => c.nodeType !== 8).map((c) => c.textContent).join(' ') }
  set textContent(v) {
    if (this.nodeType === 3) { this._text = String(v); return }
    this.childNodes.forEach((c) => { c.parentNode = null })
    this.childNodes = []
    if (v !== '' && v != null) { const t = new FakeNode('#text', 3); t._text = String(v); t.parentNode = this; this.childNodes.push(t) }
  }

  get classList() {
    const self = this
    const set = () => new Set((self.attrs.class || '').split(/\s+/).filter(Boolean))
    return {
      add: (...c) => { const s = set(); c.forEach((x) => s.add(x)); self.attrs.class = [...s].join(' ') },
      remove: (...c) => { const s = set(); c.forEach((x) => s.delete(x)); self.attrs.class = [...s].join(' ') },
      contains: (c) => set().has(c),
      toggle: () => {},
    }
  }

  insertBefore(n, ref) {
    if (n.parentNode) n.parentNode.removeChild(n)
    const i = ref ? this.childNodes.indexOf(ref) : -1
    if (i < 0) this.childNodes.push(n)
    else this.childNodes.splice(i, 0, n)
    n.parentNode = this
    return n
  }

  appendChild(n) { return this.insertBefore(n, null) }
  removeChild(n) { const i = this.childNodes.indexOf(n); if (i >= 0) this.childNodes.splice(i, 1); n.parentNode = null; return n }
  remove() { this.parentNode?.removeChild(this) }
  setAttribute(k, v) { this.attrs[k] = String(v) }
  getAttribute(k) { return k in this.attrs ? this.attrs[k] : null }
  hasAttribute(k) { return k in this.attrs }
  removeAttribute(k) { delete this.attrs[k] }
  addEventListener(t, fn) { (this.listeners[t] ||= []).push(fn) }
  removeEventListener(t, fn) { this.listeners[t] = (this.listeners[t] || []).filter((f) => f !== fn) }
  dispatchEvent(e) { (this.listeners[e.type] || []).slice().forEach((f) => f(e)); return true }
  cloneNode() { const n = new FakeNode(this.tagName, this.nodeType); n._text = this._text; return n }
  querySelector() { return null }
  querySelectorAll() { return [] }
  closest() { return null }
  contains() { return false }
  matches() { return false }
  getBoundingClientRect() { return { x: 0, y: 0, width: 0, height: 0, top: 0, left: 0, right: 0, bottom: 0 } }
  focus() {}
  blur() {}
  scrollIntoView() {}
  getRootNode() { return this }
}

export const doc = new FakeNode('#document', 9)
doc.createElement = (t) => new FakeNode(t)
doc.createElementNS = (_ns, t) => new FakeNode(t)
doc.createTextNode = (t) => { const n = new FakeNode('#text', 3); n._text = String(t); return n }
doc.createComment = (t) => { const n = new FakeNode('#comment', 8); n._text = String(t); return n }
doc.createDocumentFragment = () => new FakeNode('#fragment', 11)
doc.body = new FakeNode('body')
doc.head = new FakeNode('head')
doc.documentElement = new FakeNode('html')
doc.activeElement = null
doc.createTreeWalker = () => ({ currentNode: null, nextNode: () => null })
doc.importNode = (n) => n
doc.addEventListener = () => {}
doc.removeEventListener = () => {}
doc.querySelector = (sel) => (sel === 'body' ? doc.body : null)

const globals = {
  document: doc,
  window: globalThis,
  matchMedia: () => ({ matches: false, addEventListener() {}, removeEventListener() {}, addListener() {}, removeListener() {} }),
  ResizeObserver: class { observe() {} unobserve() {} disconnect() {} },
  IntersectionObserver: class { observe() {} unobserve() {} disconnect() {} },
  MutationObserver: class { observe() {} disconnect() {} },
  requestAnimationFrame: (f) => setTimeout(() => f(Date.now()), 0),
  cancelAnimationFrame: (id) => clearTimeout(id),
  getComputedStyle: () => ({ getPropertyValue: () => '' }),
  HTMLElement: FakeNode,
  Element: FakeNode,
  Node: FakeNode,
  CustomEvent: class { constructor(type, init) { this.type = type; this.detail = init?.detail } },
  Event: class { constructor(type) { this.type = type } },
  localStorage: { getItem: () => null, setItem() {}, removeItem() {} },
  sessionStorage: { getItem: () => null, setItem() {}, removeItem() {} },
  addEventListener: () => {},
  removeEventListener: () => {},
  scrollTo: () => {},
  scroll: () => {},
  Document: class {},
  DocumentFragment: class {},
  CSSStyleSheet: class { replaceSync() {} },
  HTMLTemplateElement: class {},
  HTMLButtonElement: class {},
  HTMLAnchorElement: class {},
  HTMLInputElement: class {},
  HTMLTextAreaElement: class {},
  HTMLSelectElement: class {},
  SVGElement: class {},
  ShadowRoot: class {},
  customElements: { get: () => undefined, define() {}, whenDefined: () => Promise.resolve() },
}
for (const [key, value] of Object.entries(globals)) globalThis[key] = value
if (!globalThis.navigator) globalThis.navigator = { userAgent: 'node', language: 'zh-CN', languages: ['zh-CN'] }

// ---- looking at what a page drew ----
export function walk(node, visit) {
  visit(node)
  for (const child of node.childNodes || []) walk(child, visit)
}

export const findAll = (root, test) => { const out = []; walk(root, (n) => { if (n.nodeType === 1 && test(n)) out.push(n) }); return out }
export const textOf = (root) => root.textContent.replace(/\s+/g, ' ').trim()
export const isDisabled = (node) => node.disabled === true || (node.attrs.disabled !== undefined && node.attrs.disabled !== 'false') || node.attrs['aria-disabled'] === 'true'
// a disabled control takes no click, as in a browser
export function click(node) {
  if (isDisabled(node)) return false
  const event = { type: 'click', target: node, currentTarget: node, preventDefault() {}, stopPropagation() {}, stopImmediatePropagation() {} }
  node.dispatchEvent(event)
}
