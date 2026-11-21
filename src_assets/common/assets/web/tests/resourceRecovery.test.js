import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import { runInNewContext } from 'node:vm'

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
      setAttribute(name, value) { this.attributes[name] = value },
      appendChild(child) {
        if (child.parent) child.parent.children = child.parent.children.filter((existing) => existing !== child)
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
  browser.fire('vite:preloadError')
  assert.equal(browser.html.children.length, 1)
  const notice = browser.html.children[0]
  assert.equal(notice.attributes.role, 'alert')
  assert.match(notice.children[0].textContent, /unsaved changes/i)
  assert.equal(browser.reloadCount(), 0)

  notice.children[2].listeners.click()
  assert.equal(browser.html.children.length, 0)
  browser.fire('vite:preloadError')
  assert.equal(browser.reloadCount(), 0)
  browser.html.children[0].children[1].listeners.click()
  assert.equal(browser.reloadCount(), 1)
})

test('a load failure stays actionable when a modal dialog closes', () => {
  const browser = page(true)
  browser.fire('vite:preloadError')
  assert.equal(browser.modal.children.length, 1)
  const notice = browser.modal.children[0]
  browser.modal.listeners.close()
  assert.equal(browser.modal.children.length, 0)
  assert.equal(browser.html.children[1], notice)
  notice.children[2].listeners.click()
  assert.equal(browser.reloadCount(), 0)
})

test('module preloads are reported while unrelated resource errors do not interrupt the page', () => {
  const browser = page()
  browser.fire('error', { target: { tagName: 'IMG', src: '/image.png' } })
  assert.equal(browser.html.children.length, 0)
  browser.fire('error', { target: { tagName: 'LINK', rel: 'modulepreload' } })
  assert.equal(browser.html.children.length, 1)
})
