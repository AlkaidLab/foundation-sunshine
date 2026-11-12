// A slot given twice to one component: Vue keeps one of them and the other is lost without a message (the 串流 tab lost the shell utilities when 添加应用 was given as a second #top).
// Every .vue file of the Web UI is parsed; among the <template #name> children of one element a name may repeat only as the branches of one v-if / v-else-if / v-else chain.
import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import { parse } from '@vue/compiler-sfc'

const root = new URL('../', import.meta.url)
const files = []
const walk = (dir) => { for (const e of fs.readdirSync(dir, { withFileTypes: true })) { const p = new URL(e.name + (e.isDirectory() ? '/' : ''), dir); if (e.isDirectory()) { if (e.name !== 'node_modules' && e.name !== 'tests') walk(p) } else if (e.name.endsWith('.vue')) files.push(p) } }
walk(root)

const slotOf = (el) => { for (const p of el.props || []) if (p.type === 7 && p.name === 'slot') return p.arg ? (p.arg.isStatic ? p.arg.content : '<dynamic>') : 'default'; return null }
const condOf = (el) => (el.props || []).some((p) => p.type === 7 && ['if', 'else-if', 'else'].includes(p.name))

/** The duplicate slot names among the children of every element of a template AST: [{ name, tag }] */
export function duplicateSlots(node, out = []) {
  const kids = (node.children || []).filter((c) => c.type === 1)
  const byName = new Map()
  for (const k of kids) { if (k.tag !== 'template') continue; const n = slotOf(k); if (n === null || n === '<dynamic>') continue; (byName.get(n) || byName.set(n, []).get(n)).push(k) }
  for (const [name, list] of byName) if (list.length > 1 && !list.every(condOf)) out.push({ name, tag: node.tag })
  // a name with one unconditional use and a conditional one is the bug: the chain rule needs every use in a chain; a lone v-if next to a plain one is not a chain
  for (const [name, list] of byName) if (list.length > 1 && list.every(condOf)) { const heads = list.filter((k) => (k.props || []).some((p) => p.type === 7 && p.name === 'if')); if (heads.length > 1 && !out.some((o) => o.name === name && o.tag === node.tag)) { /* two separate v-if with the same slot: both may be true at once */ out.push({ name, tag: node.tag }) } }
  for (const k of node.children || []) if (k.type === 1 || k.type === 9 || k.type === 11) duplicateSlots(k, out)
  return out
}

test('the checker sees the bug it is written for', () => {
  const bad = parse('<template><A><template v-if="x" #top>1</template><template #top>2</template></A></template>').descriptor.template.ast
  assert.deepEqual(duplicateSlots(bad), [{ name: 'top', tag: 'A' }])
  const chain = parse('<template><A><template v-if="x" #top>1</template><template v-else #top>2</template></A></template>').descriptor.template.ast
  assert.deepEqual(duplicateSlots(chain), [])
})

test('no component of the Web UI gives one slot twice', () => {
  const found = []
  for (const f of files) {
    const src = fs.readFileSync(f, 'utf8'); const { descriptor } = parse(src)
    if (descriptor.template?.ast) for (const d of duplicateSlots(descriptor.template.ast)) found.push(`${f.pathname.split('/web/').pop()}: <${d.tag}> #${d.name}`)
  }
  assert.deepEqual(found, [])
})

test('the shell gives the list pane no slot content: no add button (the utilities are in the caption bar of a phone, not in the list)', () => {
  const shell = fs.readFileSync(new URL('../views/WebShell.vue', import.meta.url), 'utf8')
  assert.equal((shell.match(/#top>/g) || []).length, 0)
  assert.ok(!/m-navlist__add/.test(shell))
})
