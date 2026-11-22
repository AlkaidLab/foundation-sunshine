import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import { runInNewContext } from 'node:vm'
import { startHarness } from './helpers/editorHarness.js'

const template = readFileSync(new URL('../template_header.html', import.meta.url), 'utf8')
const boot = template.match(/<script>\s*([\s\S]*?)<\/script>/)?.[1]
assert.ok(boot, 'the resource recovery listener must run before the entry module')

function page(withModal = false) {
  const listeners = new Map()
  let reloads = 0
  function element(tagName) {
    return {
      tagName: tagName.toUpperCase(),
      children: [],
      style: {},
      attributes: {},
      listeners: {},
      popoverOpen: false,
      popoverShows: 0,
      setAttribute(name, value) { this.attributes[name] = value },
      removeAttribute(name) { delete this.attributes[name] },
      matches(selector) { return selector === ':popover-open' && this.popoverOpen },
      showPopover() { this.popoverOpen = true; this.popoverShows++ },
      appendChild(child) {
        if (child.parent) child.parent.children = child.parent.children.filter((existing) => existing !== child)
        if (child.parent) child.popoverOpen = false
        this.children.push(child)
        child.parent = this
        child.isConnected = true
      },
      addEventListener(name, callback) { this.listeners[name] = callback },
      remove() { this.parent.children = this.parent.children.filter((child) => child !== this); this.isConnected = false },
    }
  }
  const html = element('html')
  html.lang = 'en'
  const modal = withModal ? element('dialog') : null
  if (modal) html.appendChild(modal)
  const window = {
    location: { reload() { reloads++ } },
    addEventListener(name, callback) { listeners.set(name, callback) },
  }
  const document = { documentElement: html, createElement: element, querySelector() { return modal } }
  runInNewContext(boot, { document, window, navigator: { language: 'en-US' } })
  return {
    html,
    modal,
    fire(name, event = {}) { listeners.get(name)?.(event) },
    reloadCount() { return reloads },
  }
}

test('entry and dynamic module failures offer an explicit reload without losing edits automatically', () => {
  const browser = page()
  browser.fire('error', { target: { tagName: 'SCRIPT', type: 'module', src: '/assets/generated/old.js' } })
  const failure = new Error('preload failed')
  browser.fire('vite:preloadError', { payload: failure })
  browser.fire('unhandledrejection', { reason: failure })
  assert.equal(browser.html.children.length, 1)
  const notice = browser.html.children[0]
  assert.equal(notice.attributes.role, 'alert')
  assert.match(notice.children[0].textContent, /unsaved changes/i)
  assert.equal(browser.reloadCount(), 0)

  notice.children[2].listeners.click()
  assert.equal(browser.html.children.length, 0)
  const anotherFailure = new Error('another preload failed')
  browser.fire('vite:preloadError', { payload: anotherFailure })
  browser.fire('unhandledrejection', { reason: anotherFailure })
  assert.equal(browser.reloadCount(), 0)
  browser.html.children[0].children[1].listeners.click()
  assert.equal(browser.reloadCount(), 1)
})

test('a load failure stays actionable when a modal dialog closes', () => {
  const browser = page(true)
  const failure = new Error('preload failed')
  browser.fire('vite:preloadError', { payload: failure })
  browser.fire('unhandledrejection', { reason: failure })
  assert.equal(browser.modal.children.length, 1)
  const notice = browser.modal.children[0]
  assert.equal(notice.popoverShows, 1)
  browser.modal.listeners.close()
  assert.equal(browser.modal.children.length, 0)
  assert.equal(browser.html.children[1], notice)
  assert.equal(notice.popoverShows, 2)
  notice.children[2].listeners.click()
  assert.equal(browser.reloadCount(), 0)
})

test('speculative module preloads and unrelated errors do not interrupt the page', () => {
  const browser = page()
  browser.fire('error', { target: { tagName: 'IMG', src: '/image.png' } })
  assert.equal(browser.html.children.length, 0)
  browser.fire('error', { target: { tagName: 'LINK', rel: 'modulepreload' } })
  assert.equal(browser.html.children.length, 0)
})

test('a failed preload followed by a successful lazyPane retry shows no error', async (t) => {
  const browser = page()
  // The existing Vite harness loads the real Vue async component and its loader.
  const harness = await startHarness()
  t.after(() => harness.close())
  const { lazyPane } = await harness.load('/utils/lazyPane.js')
  const first = new Error('temporary failure')
  const component = { name: 'Loaded' }
  let attempts = 0
  const pane = lazyPane(() => {
    if (++attempts === 1) {
      browser.fire('vite:preloadError', { payload: first })
      return Promise.reject(first)
    }
    return Promise.resolve(component)
  })
  assert.equal(await pane.__asyncLoader(), component)
  assert.equal(attempts, 2)
  assert.equal(browser.html.children.length, 0)

  // A rejected loader must actually render the error component after both
  // attempts, including the reload action, rather than leaving an empty pane.
  t.mock.method(console, 'error', () => {})
  let failures = 0
  const failed = lazyPane(() => { failures++; return Promise.reject(new Error('unavailable')) })
  const rendered = await harness.mount(failed, {})
  assert.equal(failures, 2)
  assert.match(rendered.text(), /shell\.pane_failed/)
  assert.equal(rendered.buttons('shell.pane_reload').length, 1)
  rendered.unmount()
})
