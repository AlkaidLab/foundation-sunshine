import test from 'node:test'
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'

// 关于 has two doors (see the panel's test of the same name): the footer button of the rail is a first-level page, the status pages and the doctor push it over themselves and keep Back.
const utils = await readFile(new URL('../components/layout/ShellUtils.vue', import.meta.url), 'utf8')
const shell = await readFile(new URL('../views/WebShell.vue', import.meta.url), 'utf8')
const doctor = await readFile(new URL('../views/Doctor.vue', import.meta.url), 'utf8')

test('the footer buttons go to a page as a first-level page (no Back); the other doors push 关于', () => {
  assert.ok(utils.includes('shell.nav.go(id, undefined, { root: true })'))
  assert.ok(shell.includes("about: { rail: 'help', rootRail: 'about', list: false, push: true"), 'the route knows its rail as a root page')
  assert.ok(shell.includes('push: p.push, rootRail: p.rootRail }'), 'and the route table carries it to the back stack')
  assert.ok(doctor.includes("update: () => shell?.nav.go('about')"), 'the doctor pushes it: Back returns to the doctor')
  assert.ok(shell.includes("openAbout: () => jumpNav.go('about')"), 'the status page pushes it (as a jump: the chip names the way back)')
})
