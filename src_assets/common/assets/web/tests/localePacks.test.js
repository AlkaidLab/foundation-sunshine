import assert from 'node:assert/strict'
import test from 'node:test'
import { computed } from 'vue'
import { createLocalePacks } from '../config/localePacks.js'

test('shared texts load only the requested locale and reuse its pending and completed read', async () => {
  let reads = 0
  const gate = Promise.withResolvers()
  const packs = createLocalePacks({ zh_TW: () => { reads++; return gate.promise } })
  const en = { title: 'Title', hint: 'Hint' }
  const zh = { title: '标题', hint: '提示' }
  assert.equal(packs.textTable('guide', 'en', en, zh), en)
  assert.equal(packs.textTable('guide', 'zh-CN', en, zh), zh)
  assert.equal(reads, 0)
  const text = computed(() => packs.textTable('guide', 'zh-Hant-TW', en, zh))
  assert.equal(text.value, zh)
  const pending = packs.loadLocale('zh_TW')
  assert.equal(pending, packs.loadLocale('zh-Hant-TW'))
  gate.resolve({ guide: { title: '標題' } })
  await pending
  assert.deepEqual(text.value, { title: '標題', hint: '提示' })
  await packs.loadLocale('zh_TW')
  assert.equal(reads, 1)
})

test('a failed language chunk uses the base texts until an explicit retry succeeds', async (t) => {
  t.mock.method(console, 'warn', () => {})
  let reads = 0
  const packs = createLocalePacks({ fr: async () => {
    if (++reads === 1) throw new Error('offline')
    return { guide: { title: 'Guide' } }
  } })
  await packs.loadLocale('fr')
  assert.equal(packs.partialFor('fr', 'guide'), null)
  assert.equal(reads, 1)
  await packs.loadLocale('fr')
  assert.deepEqual(packs.partialFor('fr', 'guide'), { title: 'Guide' })
})
