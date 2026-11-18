// The client download page's community links and project links.
// Every address is one of the host repository's own (README.md, config/resources.js); a test keeps the list complete.

import { QUICK_START_RESOURCES, resolveRandomResourceHref } from './resources.js'

const REPO = 'https://github.com/AlkaidLab/foundation-sunshine'
const COMMUNITY_GROUP = QUICK_START_RESOURCES.find((resource) => resource.id === 'community-group')

export const PROJECT_LINKS = Object.freeze([
  { id: 'site', icon: 'public', url: 'https://www.alkaidlab.com/', zh: 'https://www.alkaidlab.cn/' },
  { id: 'docs', icon: 'menu_book', url: 'https://www.alkaidlab.com/docs/', zh: 'https://www.alkaidlab.cn/docs/' },
  { id: 'github', icon: 'code', url: REPO },
  { id: 'changelog', icon: 'history', url: `${REPO}/releases` },
  { id: 'license', icon: 'balance', url: `${REPO}/blob/master/LICENSE` },
  { id: 'qq', icon: 'groups', url: COMMUNITY_GROUP.href },
  { id: 'issues', icon: 'bug_report', url: `${REPO}/issues` },
])

export const PARTNER_CLIENTS = Object.freeze([
  { id: 'voidlink', name: 'VoidLink', os: 'iOS / iPadOS / Apple TV', icon: 'tablet_mac', links: [
    { id: 'project', url: 'https://github.com/The-Fried-Fish/VoidLink-previously-moonlight-zwm#readme' },
    { id: 'store-cn', url: 'https://apps.apple.com/cn/app/voidlink/id6747717070' },
    { id: 'store-global', url: 'https://apps.apple.com/us/app/voidlink-extreme/id6755103808' },
  ] },
  { id: 'crown', name: '王冠版', os: 'Android', icon: 'android', links: [
    { id: 'download', url: 'https://github.com/WACrown/moonlight-android/releases' },
    { id: 'project', url: 'https://github.com/WACrown/moonlight-android#readme' },
  ] },
  { id: 'baozi', name: '包子版', os: 'HarmonyOS', icon: 'mobile', links: [{ id: 'project', url: 'https://gitee.com/smdsbz/moonlight-ohos' }] },
])

/**
 * Community links that have no card of their own among the platforms: their editions of Android and HarmonyOS (VoidLink is the iOS and Apple TV card of
 * the platforms, marked as a partner). `types` are the device types of the page's selector they belong to.
 */
export const PARTNER_CARDS = Object.freeze([
  { id: 'crown', name: '王冠版', os: 'Android', icon: 'android', types: ['phone', 'tablet'], links: PARTNER_CLIENTS.find((c) => c.id === 'crown').links },
  { id: 'baozi', name: '包子版', os: 'HarmonyOS', icon: 'mobile', types: ['phone', 'tablet'], links: PARTNER_CLIENTS.find((c) => c.id === 'baozi').links },
])

const TEXTS = {
  en: { partner: 'Partner', partners: 'Partner clients', project: 'Project and community', site: 'Website', docs: 'Documentation', github: 'GitHub', changelog: 'Changelog', license: 'Licence', qq: 'QQ group', issues: 'Report an issue',
    'store-cn': 'App Store (China)', 'store-global': 'App Store (US)', download: 'Download', projectPage: 'Project' },
  zh: { partner: '合作', partners: '合作客户端', project: '项目与社区', site: '官网', docs: '文档', github: 'GitHub', changelog: '更新日志', license: '许可证', qq: 'QQ 交流群', issues: '问题反馈',
    'store-cn': 'App Store（中国区）', 'store-global': 'App Store（美区／外区）', download: '下载', projectPage: '项目介绍' },
}
export const communityTexts = (locale = 'en') => (/^zh/i.test(String(locale)) ? TEXTS.zh : TEXTS.en)
export const linkLabel = (texts, link) => (link.id === 'project' ? texts.projectPage : texts[link.id] || link.id)
export const resolveLink = (link, locale = 'en', randomValue) => {
  if (link.id === 'qq') return resolveRandomResourceHref(COMMUNITY_GROUP, randomValue)
  return /^zh/i.test(String(locale)) && link.zh ? link.zh : link.url
}
