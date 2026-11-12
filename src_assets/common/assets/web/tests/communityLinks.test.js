import './helpers/foundationUi.mjs'
import test from 'node:test'
import assert from 'node:assert/strict'
import { PARTNER_CARDS, PARTNER_CLIENTS, PROJECT_LINKS, communityTexts, linkLabel, resolveLink } from '../config/communityLinks.js'
import { CLIENT_PLATFORMS } from '../config/clientDownloads.js'
const { CLIENTS, clientSections, clientsForOs } = await import('foundation-ui/clients')

// every address the About page listed before the partner clients and the project links moved to the download page
const FROM_ABOUT = [
  'https://www.alkaidlab.com/', 'https://www.alkaidlab.cn/', 'https://www.alkaidlab.com/docs/', 'https://www.alkaidlab.cn/docs/',
  'https://github.com/AlkaidLab/foundation-sunshine', 'https://github.com/AlkaidLab/foundation-sunshine/releases', 'https://github.com/AlkaidLab/foundation-sunshine/blob/master/LICENSE',
  'https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi', 'https://github.com/AlkaidLab/foundation-sunshine/issues',
  'https://github.com/The-Fried-Fish/VoidLink-previously-moonlight-zwm#readme', 'https://apps.apple.com/app/voidlink/id6747717070', 'https://apps.apple.com/app/voidlink-extreme/id6755103808',
  'https://testflight.apple.com/join/k7gs61mZ', 'https://github.com/WACrown/moonlight-android/releases', 'https://github.com/WACrown/moonlight-android#readme', 'https://gitee.com/smdsbz/moonlight-ohos',
]

test('the download page keeps every link the About page had', () => {
  const have = new Set([...PROJECT_LINKS.flatMap((l) => [l.url, l.zh]), ...PARTNER_CLIENTS.flatMap((c) => c.links.map((l) => l.url))].filter(Boolean))
  for (const url of FROM_ABOUT) assert.ok(have.has(url), `missing ${url}`)
})

test('every link has a label in both languages and the Chinese mirror is used in Chinese', () => {
  for (const locale of ['en', 'zh']) {
    const texts = communityTexts(locale)
    for (const link of [...PROJECT_LINKS, ...PARTNER_CLIENTS.flatMap((c) => c.links)]) assert.ok(linkLabel(texts, link), `${locale} ${link.id}`)
  }
  assert.equal(resolveLink(PROJECT_LINKS[0], 'zh-CN'), 'https://www.alkaidlab.cn/')
  assert.equal(resolveLink(PROJECT_LINKS[0], 'en'), 'https://www.alkaidlab.com/')
})

test('the partner tag follows the one client list: the team\'s four clients (HarmonyOS V+ is official) are not partners, every other client is', () => {
  const { first, partner } = clientSections()
  assert.deepEqual(first.map((c) => c.id), ['vplus-desktop', 'vplus-android', 'macos-enhanced', 'vplus-harmony'])
  for (const client of CLIENTS) for (const target of client.targets) {
    const platform = CLIENT_PLATFORMS.find((p) => p.id === target.platform)
    const card = PARTNER_CARDS.find((c) => c.id === target.platform)
    assert.ok(platform || card, `${client.id}: ${target.platform} has a platform card or a partner card`)
    assert.equal(client.group === 'partner', Boolean(platform?.friendLink || card), `${client.id}/${target.platform} partner badge`)
  }
  // the editions of the Crown and OpenHarmony are partners' and are listed on the system they run on
  assert.deepEqual(PARTNER_CARDS.map((c) => c.id).sort(), partner.filter((c) => ['crown', 'baozi'].includes(c.id)).map((c) => c.id).sort())
  assert.ok(clientsForOs('android').some((x) => x.target.platform === 'crown'))
})

test('every client of the list reaches the project or the store its link points at', () => {
  const have = new Set([...PARTNER_CLIENTS.flatMap((c) => c.links.map((l) => l.url)), ...CLIENT_PLATFORMS.flatMap((p) => [p.project, p.release, ...(p.links || []).map((l) => l.url), ...(p.alternatives || []).flatMap((a) => [a.project, a.release])]), ...PARTNER_CARDS.flatMap((c) => c.links.map((l) => l.url))].filter(Boolean))
  const norm = (u) => String(u).replace(/#readme$/, '').replace(/\/$/, '')
  const reach = (url) => [...have].some((h) => norm(h) === norm(url) || norm(h).startsWith(norm(url) + '/') || norm(url).startsWith(norm(h) + '/'))
  for (const client of CLIENTS) assert.ok(reach(client.url) || (client.zhUrl && reach(client.zhUrl)) || client.id === 'vplus-harmony' || client.id === 'voidlink', `${client.id}: ${client.url}`)
})
