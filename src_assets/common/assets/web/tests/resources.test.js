import test from 'node:test'
import assert from 'node:assert/strict'
import {
  CLIENT_RESOURCES,
  OFFICIAL_RESOURCES,
  resolveResourceHref,
} from '../config/resources.js'

test('uses mainland links only for simplified Chinese', () => {
  const website = OFFICIAL_RESOURCES.find((resource) => resource.id === 'official-website')
  const voidlink = CLIENT_RESOURCES.find((resource) => resource.id === 'voidlink')

  assert.equal(resolveResourceHref(website, 'zh'), 'https://www.alkaidlab.cn/')
  assert.equal(
    resolveResourceHref(voidlink, 'zh'),
    'https://apps.apple.com/cn/app/voidlink/id6747717070',
  )
})

test('uses international links for every other locale', () => {
  const website = OFFICIAL_RESOURCES.find((resource) => resource.id === 'official-website')
  const voidlink = CLIENT_RESOURCES.find((resource) => resource.id === 'voidlink')

  for (const locale of ['en', 'en_US', 'zh_TW', 'ja']) {
    assert.equal(resolveResourceHref(website, locale), 'https://www.alkaidlab.com/')
    assert.equal(
      resolveResourceHref(voidlink, locale),
      'https://apps.apple.com/us/app/voidlink-extreme/id6755103808',
    )
  }
})
