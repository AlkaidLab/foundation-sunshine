export const HARMONY_CLIENT_URL = 'https://github.com/AlkaidLab/moonlight-harmony'

const ALKAIDLAB_WEBSITE_URL = 'https://www.alkaidlab.com/'
const ALKAIDLAB_WEBSITE_ZH_URL = 'https://www.alkaidlab.cn/'
const VOIDLINK_APP_STORE_URL = 'https://apps.apple.com/us/app/voidlink-extreme/id6755103808'
const VOIDLINK_APP_STORE_ZH_URL = 'https://apps.apple.com/cn/app/voidlink/id6747717070'
const STREAMING_GROUP_URLS = [
  'https://qm.qq.com/q/AfMQoyKrkc',
  'https://qm.qq.com/q/vIVhpjDMic',
]

export const OFFICIAL_RESOURCES = [
  {
    id: 'official-website',
    href: ALKAIDLAB_WEBSITE_URL,
    zhHref: ALKAIDLAB_WEBSITE_ZH_URL,
    icon: 'public',
    titleKey: 'resource_card.official_website_title',
    variant: 'accent',
  },
  {
    id: 'foundation-github',
    href: 'https://github.com/AlkaidLab/foundation-sunshine',
    icon: 'code',
    title: 'Foundation Sunshine',
    variant: 'github',
    arrowIcon: 'star',
    arrowClass: 'text-warning',
  },
]

export const QUICK_START_RESOURCES = [
  {
    id: 'tutorial',
    href: 'https://www.alkaidlab.com/docs/',
    zhHref: 'https://www.alkaidlab.cn/docs/',
    icon: 'description',
    titleKey: 'resource_card.tutorial',
    variant: 'accent',
  },
  {
    id: 'community-group',
    href: STREAMING_GROUP_URLS[0],
    randomHrefs: STREAMING_GROUP_URLS,
    icon: 'groups',
    titleKey: 'resource_card.join_group',
    variant: 'accent-alt',
  },
]

export const CLIENT_RESOURCES = [
  {
    id: 'android-vplus',
    client: 'official',
    href: 'https://github.com/qiin2333/moonlight-vplus',
    icon: 'android',
    titleKey: 'resource_card.android_vplus_title',
    description: 'Android / Android TV',
    variant: 'android',
  },
  {
    id: 'harmony-vplus',
    client: 'official',
    href: '#harmony-client',
    icon: 'mobile',
    titleKey: 'resource_card.harmony_client',
    description: 'HarmonyOS NEXT',
    variant: 'harmony',
    arrowIcon: 'chevron_right',
    action: 'harmony',
  },
  {
    id: 'moonlight-desktop',
    client: 'official',
    href: 'https://github.com/qiin2333/moonlight-qt',
    icon: 'desktop_windows',
    titleKey: 'resource_card.moonlight_pc_title',
    description: 'Windows / macOS / Linux',
    variant: 'desktop',
  },
  {
    id: 'moonlight-macos',
    client: 'official',
    href: 'https://github.com/skyhua0224/moonlight-macos-enhanced',
    icon: 'laptop_mac',
    titleKey: 'resource_card.moonlight_macos_enhanced',
    descriptionKey: 'resource_card.moonlight_macos_enhanced_desc',
    variant: 'apple',
  },
  {
    id: 'voidlink',
    client: 'partner',
    href: VOIDLINK_APP_STORE_URL,
    zhHref: VOIDLINK_APP_STORE_ZH_URL,
    icon: 'mobile',
    titleKey: 'resource_card.voidlink_title',
    description: 'iOS / iPadOS / Apple TV',
    variant: 'apple',
  },
]

export const COMMUNITY_RESOURCES = [
  {
    id: 'crown-edition',
    client: 'partner',
    href: 'https://github.com/WACrown/moonlight-android',
    icon: 'star',
    titleKey: 'resource_card.crown_edition',
    descriptionKey: 'resource_card.crown_edition_desc',
    variant: 'android',
  },
  {
    id: 'moonlight-ohos',
    client: 'partner',
    href: 'https://gitee.com/smdsbz/moonlight-ohos',
    icon: 'mobile',
    titleKey: 'resource_card.moonlight_ohos',
    descriptionKey: 'resource_card.moonlight_ohos_desc',
    variant: 'harmony',
  },
]

export const HOME_RESOURCE_GROUPS = [
  {
    id: 'official',
    titleKey: 'resource_card.official_website',
    icon: 'public',
    items: OFFICIAL_RESOURCES,
  },
  {
    id: 'quick-start',
    titleKey: 'resource_card.quick_start',
    icon: 'rocket_launch',
    items: QUICK_START_RESOURCES,
  },
  {
    id: 'clients',
    titleKey: 'resource_card.client_downloads',
    icon: 'download',
    items: CLIENT_RESOURCES,
  },
  {
    id: 'community',
    titleKey: 'resource_card.third_party_moonlight',
    icon: 'hub',
    items: COMMUNITY_RESOURCES,
  },
]

export const LEGAL_RESOURCES = [
  {
    id: 'license',
    href: 'https://github.com/AlkaidLab/foundation-sunshine/blob/master/LICENSE',
    icon: 'description',
    titleKey: 'resource_card.license',
    descriptionKey: 'resource_card.view_license',
    variant: 'accent',
  },
  {
    id: 'third-party-notice',
    href: 'https://github.com/AlkaidLab/foundation-sunshine/blob/master/NOTICE',
    icon: 'warning',
    titleKey: 'resource_card.third_party_notice',
    variant: 'accent',
  },
]

export const FEATURED_RESOURCES = [
  {
    id: 'alkaidlab',
    href: ALKAIDLAB_WEBSITE_URL,
    zhHref: ALKAIDLAB_WEBSITE_ZH_URL,
    imageSrc: '/images/logo-alkaidlab.png',
    imageAlt: 'AlkaidLab',
    titleKey: 'resource_card.official_website_title',
    descriptionKey: 'resource_card.official_website_desc',
    variant: 'accent',
  },
  {
    id: 'natpierce',
    href: 'https://docs.qq.com/aio/DRFVhWERDaFhKd1ZE',
    imageSrc: '/images/logo-natpierce.png',
    imageAlt: '皎月连',
    titleKey: 'resource_card.jiaoyuelian_title',
    descriptionKey: 'resource_card.jiaoyuelian_desc',
    variant: 'moonlink',
  },
]

export function resolveResourceText(translate, resource, field) {
  const key = resource[`${field}Key`]
  return key ? translate(key) : resource[field] || ''
}

export function resolveResourceHref(resource, locale) {
  return locale === 'zh' ? resource.zhHref || resource.href : resource.href
}

export function resolveRandomResourceHref(resource, randomValue = Math.random()) {
  const hrefs = resource.randomHrefs
  if (!Array.isArray(hrefs) || hrefs.length === 0) return resolveResourceHref(resource)
  return hrefs[randomValue < 0.1 ? 0 : 1] || hrefs[0]
}
