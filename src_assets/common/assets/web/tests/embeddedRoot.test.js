import test from 'node:test'
import assert from 'node:assert/strict'
import { applyRootMessage, themeCssOk } from '../utils/embeddedRoot.js'

const fakeRoot = () => {
  const style = new Map(); const classes = new Set(); const attrs = new Map()
  return { style: { setProperty: (n, v) => style.set(n, v), removeProperty: (n) => style.delete(n) }, classList: { toggle: (c, on) => (on ? classes.add(c) : classes.delete(c)) }, setAttribute: (n, v) => attrs.set(n, v), _: { style, classes, attrs } }
}

test('colour tokens, the picture flag and the scheme are applied; everything else is dropped', () => {
  const root = fakeRoot()
  const names = applyRootMessage(root, { type: 'foundation-root', props: { '--md-sys-color-primary': '#123456', '--m-scrim': '0.4', '--m-card': 'color-mix(in srgb, #fff 96%, transparent)', '--m-wallpaper': 'url(x)', '--evil': 'red', '--md-sys-color-x': 'url(https://x)', 'background': 'red' }, classes: ['m-has-wallpaper', 'x'], theme: 'dark' })
  assert.deepEqual([...root._.style.keys()].sort(), ['--m-card', '--m-scrim', '--md-sys-color-primary'])
  assert.deepEqual([...root._.classes], ['m-has-wallpaper'])
  assert.equal(root._.attrs.get('data-bs-theme'), 'dark')
  assert.equal(names.size, 3)
})

test('a name the next message no longer has is removed; other messages change nothing', () => {
  const root = fakeRoot()
  let names = applyRootMessage(root, { type: 'foundation-root', props: { '--m-scrim': '0.4', '--m-card': '#fff' }, classes: [], theme: 'light' })
  names = applyRootMessage(root, { type: 'foundation-root', props: { '--m-scrim': '0.5' }, classes: [], theme: 'light' }, names)
  assert.deepEqual([...root._.style.entries()], [['--m-scrim', '0.5']])
  assert.equal(applyRootMessage(root, { type: 'other' }, names), names)
})

test('the theme text of the panel is accepted only when every line is a selector or a colour declaration', () => {
  const css = ":root, [data-bs-theme='light'], [data-theme='light'] {\n--md-sys-color-primary: #123456;\n--m-x: color-mix(in srgb, #fff 90%, transparent);\n}\n[data-bs-theme='dark'], [data-theme='dark'] {\n--md-sys-color-surface: #0e0e12;\n}\n"
  assert.equal(themeCssOk(css), true)
  assert.equal(themeCssOk(css + 'body { background: url(https://x) }'), false)
  assert.equal(themeCssOk('@import url(https://x);'), false)
  assert.equal(themeCssOk(":root {\n--a: url(x);\n}"), false)
})

test('the list pane choice of the panel is applied; anything else is ignored', () => {
  const root = fakeRoot()
  applyRootMessage(root, { type: 'foundation-root', props: {}, classes: [], theme: 'light', list: 'one' })
  assert.equal(root._.attrs.get('data-list'), 'one')
  applyRootMessage(root, { type: 'foundation-root', props: {}, classes: [], theme: 'light', list: 'step' })
  assert.equal(root._.attrs.get('data-list'), 'one', 'a step (or anything else) changes nothing')
  applyRootMessage(root, { type: 'foundation-root', props: {}, classes: [], theme: 'light', list: 'merged' })
  assert.equal(root._.attrs.get('data-list'), 'merged')
})
