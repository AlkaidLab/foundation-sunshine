import { watch } from 'vue'
import { usePending } from 'foundation-ui/pending'

// The application list as pending changes: one entry for every application that was added, removed or changed (by name) and one for a new order.
const clone = (v) => JSON.parse(JSON.stringify(v))
const strip = (app) => { const { index, ...rest } = app || {}; return rest }
const same = (a, b) => JSON.stringify(strip(a)) === JSON.stringify(strip(b))

export function appsEntries(original, current, T) {
  const out = []
  const was = new Map(original.map((a) => [a.name, a]))
  const now = new Map(current.map((a) => [a.name, a]))
  for (const [name, app] of now) if (!was.has(name)) out.push({ key: `add:${name}`, label: `${T.add} ${name}`, before: undefined, after: clone(app), beforeText: '—', afterText: T.added })
  for (const [name, app] of was) if (!now.has(name)) out.push({ key: `del:${name}`, label: `${T.del} ${name}`, before: clone(app), after: undefined, beforeText: T.existing, afterText: T.deleted, at: original.indexOf(app) })
  for (const [name, app] of now) if (was.has(name) && !same(app, was.get(name))) out.push({ key: `mod:${name}`, label: `${T.mod} ${name}`, before: clone(was.get(name)), after: clone(app), beforeText: T.was, afterText: T.changed })
  const common = (list, other) => list.filter((a) => other.has(a.name)).map((a) => a.name)
  if (JSON.stringify(common(original, now)) !== JSON.stringify(common(current, was))) out.push({ key: 'order', label: T.order, before: common(original, now), after: common(current, was), beforeText: T.oldOrder, afterText: T.newOrder })
  return out
}

export function applyAppEntries(base, entries) {
  let list = clone(base)
  for (const e of entries) {
    const at = list.findIndex((a) => a.name === e.key.slice(e.key.indexOf(':') + 1))
    if (e.key.startsWith('add:')) { if (at < 0) list.push(clone(e.after)) } else if (e.key.startsWith('del:')) { if (at >= 0) list.splice(at, 1) } else if (e.key.startsWith('mod:')) { if (at >= 0) list[at] = clone(e.after) }
  }
  const order = entries.find((e) => e.key === 'order')
  if (order) list = [...order.after.map((n) => list.find((a) => a.name === n)).filter(Boolean), ...list.filter((a) => !order.after.includes(a.name))]
  return list
}

export function revertAppEntries(base, entries) {
  let list = clone(base)
  for (const e of entries) {
    const name = e.key.slice(e.key.indexOf(':') + 1)
    const at = list.findIndex((a) => a.name === name)
    if (e.key.startsWith('add:')) { if (at >= 0) list.splice(at, 1) } else if (e.key.startsWith('del:')) { if (at < 0) list.splice(Math.min(e.at ?? list.length, list.length), 0, clone(e.before)) } else if (e.key.startsWith('mod:')) { if (at >= 0) list[at] = clone(e.before) }
  }
  const order = entries.find((e) => e.key === 'order')
  if (order) list = [...order.before.map((n) => list.find((a) => a.name === n)).filter(Boolean), ...list.filter((a) => !order.before.includes(a.name))]
  return list
}

const texts = (locale) => (String(locale).startsWith('zh')
  ? { add: '新增', del: '删除', mod: '修改', added: '已添加', deleted: '已删除', changed: '已修改', existing: '已有', was: '原设置', order: '应用顺序', oldOrder: '原顺序', newOrder: '新顺序' }
  : { add: 'Add', del: 'Delete', mod: 'Change', added: 'Added', deleted: 'Deleted', changed: 'Changed', existing: 'Existing', was: 'Before', order: 'Application order', oldOrder: 'Old order', newOrder: 'New order' })

/** The page of the application list: reports its edits to the store, applies the ones kept from before and answers undo. */
export function useAppsPending({ apps, originalApps, locale, loaded }) {
  const pending = usePending()
  const T = () => texts(locale())
  let restored = false
  const stop = []
  const detach = pending.attach('apps', {
    revert: (key) => { apps.value = revertAppEntries(apps.value, pending.entries('apps').filter((e) => e.key === key)) },
    revertAll: () => { apps.value = clone(originalApps.value) },
  })
  stop.push(detach)
  stop.push(watch(() => (loaded.value ? JSON.stringify([apps.value.map(strip), originalApps.value.map(strip)]) : null), (json) => {
    if (json === null) return
    if (!restored) {
      restored = true
      const kept = pending.entries('apps')
      if (kept.length) { apps.value = applyAppEntries(apps.value, kept); return }
    }
    pending.sync('apps', appsEntries(originalApps.value, apps.value, T()))
  }, { immediate: true, flush: 'post' }))
  return () => stop.forEach((fn) => fn())
}
