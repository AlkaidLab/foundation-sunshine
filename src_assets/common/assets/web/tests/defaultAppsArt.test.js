import assert from 'node:assert/strict'
import { existsSync, readFileSync } from 'node:fs'
import test from 'node:test'

// the default entries that the Windows host ships: apps.json (English) and apps.zh.json (Chinese installer), their covers, and the names that follow the language
const windowsAssets = new URL('../../../../windows/assets/', import.meta.url)
const commonAssets = new URL('../../../assets/', import.meta.url)
const read = (name) => JSON.parse(readFileSync(new URL(name, windowsAssets), 'utf8'))
const pngSize = (url) => {
  const head = readFileSync(url).subarray(0, 24)
  assert.equal(head.subarray(1, 4).toString('latin1'), 'PNG')
  return [head.readUInt32BE(16), head.readUInt32BE(20)]
}
const locate = (name) => (existsSync(new URL(name, windowsAssets)) ? new URL(name, windowsAssets) : new URL(name, commonAssets))

const EXPECT = {
  desktop: { hero: 'desktop-hero.png', en: 'Desktop', zh: '桌面' },
  'big-screen': { hero: 'bigscreen-hero.png', en: 'Big Screen', zh: '大屏' },
  steam: { hero: 'steam-hero.png', en: 'Steam Big Picture', zh: 'Steam Big Picture 模式' },
  xbox: { hero: 'box-hero.png', en: 'Xbox Game', zh: 'Xbox 游戏' },
}

for (const [file, lang] of [['apps.json', 'en'], ['apps.zh.json', 'zh']]) {
  test(`${file}: every default entry is marked, ships a landscape cover, and is named in its language`, () => {
    const apps = read(file).apps
    assert.deepEqual(apps.map((app) => app['default-id']).sort(), Object.keys(EXPECT).sort())
    for (const app of apps) {
      const want = EXPECT[app['default-id']]
      assert.equal(app.hero, want.hero)
      assert.equal(app.name, want[lang])
      assert.ok(existsSync(locate(app.hero)), `${app.hero} exists`)
      const image = app['image-path']
      if (image !== 'desktop') assert.ok(existsSync(locate(image)), `${image} exists`)
    }
  })
}

test('the covers of the default entries have the aspect that they are named for', () => {
  for (const name of ['steam.png', 'box.png']) assert.deepEqual(pngSize(locate(name)), [600, 900], name)
  for (const name of ['steam-hero.png', 'box-hero.png', 'desktop-hero.png', 'bigscreen-hero.png']) assert.deepEqual(pngSize(locate(name)), [1920, 1080], name)
})

test('the art has a record: sources and terms in the assets README and the source index', () => {
  const readme = readFileSync(new URL('../../../../../assets/brand/default-apps/README.md', import.meta.url), 'utf8')
  assert.match(readme, /No AI-generated imagery/)
  assert.match(readme, /steam_brandAssets\.eps/)
  assert.match(readme, /FALLBACK/)
  const index = readFileSync(new URL('../../../../../docs/authority/source-index.tsv', import.meta.url), 'utf8')
  for (const key of ['valve-steam-brand-guidelines-20261110', 'valve-steam-brand-assets-eps-20261110', 'microsoft-trademark-brand-guidelines-20261110']) assert.ok(index.includes(key), key)
})
