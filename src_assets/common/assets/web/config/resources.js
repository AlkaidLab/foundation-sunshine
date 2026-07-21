export const HARMONY_CLIENT_URL = 'https://github.com/AlkaidLab/moonlight-harmony'

export const OFFICIAL_RESOURCES = [
  {
    id: 'official-website',
    href: 'https://www.alkaidlab.com/',
    icon: 'fas fa-globe',
    titleKey: 'resource_card.official_website_title',
    variant: 'accent',
  },
  {
    id: 'foundation-github',
    href: 'https://github.com/AlkaidLab/foundation-sunshine',
    icon: 'fab fa-github',
    title: 'Foundation Sunshine',
    descriptionKey: 'resource_card.open_source_desc',
    variant: 'github',
    arrowIcon: 'fas fa-star',
    arrowClass: 'text-warning',
  },
]

export const QUICK_START_RESOURCES = [
  {
    id: 'tutorial',
    href: 'https://docs.qq.com/aio/DSGdQc3htbFJjSFdO',
    icon: 'fas fa-file-alt',
    titleKey: 'resource_card.tutorial',
    descriptionKey: 'resource_card.tutorial_desc',
    variant: 'accent',
  },
  {
    id: 'community-group',
    href: 'https://qm.qq.com/q/3tWBFVNZ',
    icon: 'fab fa-qq',
    titleKey: 'resource_card.join_group',
    descriptionKey: 'resource_card.join_group_desc',
    variant: 'accent-alt',
  },
]

export const CLIENT_RESOURCES = [
  {
    id: 'android-vplus',
    href: 'https://github.com/qiin2333/moonlight-vplus',
    icon: 'fab fa-android',
    titleKey: 'resource_card.android_vplus_title',
    description: 'Android / Android TV',
    variant: 'android',
  },
  {
    id: 'harmony-vplus',
    href: '#harmony-client',
    icon: 'fas fa-mobile-alt',
    titleKey: 'resource_card.harmony_client',
    description: 'HarmonyOS NEXT',
    variant: 'harmony',
    arrowIcon: 'fas fa-chevron-right',
    action: 'harmony',
  },
  {
    id: 'moonlight-desktop',
    href: 'https://github.com/qiin2333/moonlight-qt',
    icon: 'fas fa-desktop',
    titleKey: 'resource_card.moonlight_pc_title',
    description: 'Windows / macOS / Linux',
    variant: 'desktop',
  },
  {
    id: 'moonlight-macos',
    href: 'https://github.com/skyhua0224/moonlight-macos-enhanced',
    icon: 'fab fa-apple',
    titleKey: 'resource_card.moonlight_macos_enhanced',
    descriptionKey: 'resource_card.moonlight_macos_enhanced_desc',
    variant: 'apple',
  },
  {
    id: 'voidlink',
    href: 'https://apps.apple.com/cn/app/voidlink/id6747717070',
    icon: 'fab fa-apple',
    titleKey: 'resource_card.voidlink_title',
    description: 'iOS / iPadOS',
    variant: 'apple',
  },
]

export const COMMUNITY_RESOURCES = [
  {
    id: 'crown-edition',
    href: 'https://github.com/WACrown/moonlight-android',
    icon: 'fas fa-crown',
    titleKey: 'resource_card.crown_edition',
    descriptionKey: 'resource_card.crown_edition_desc',
    variant: 'android',
  },
  {
    id: 'moonlight-ohos',
    href: 'https://gitee.com/smdsbz/moonlight-ohos',
    icon: 'fas fa-mobile-alt',
    titleKey: 'resource_card.moonlight_ohos',
    descriptionKey: 'resource_card.moonlight_ohos_desc',
    variant: 'harmony',
  },
]

export const HOME_RESOURCE_GROUPS = [
  {
    id: 'official',
    titleKey: 'resource_card.official_website',
    icon: 'fas fa-globe',
    items: OFFICIAL_RESOURCES,
  },
  {
    id: 'quick-start',
    titleKey: 'resource_card.quick_start',
    icon: 'fas fa-rocket',
    items: QUICK_START_RESOURCES,
  },
  {
    id: 'clients',
    titleKey: 'resource_card.client_downloads',
    icon: 'fas fa-download',
    items: CLIENT_RESOURCES,
  },
  {
    id: 'community',
    titleKey: 'resource_card.third_party_moonlight',
    icon: 'fas fa-code-branch',
    items: COMMUNITY_RESOURCES,
  },
]

export const LEGAL_RESOURCES = [
  {
    id: 'license',
    href: 'https://github.com/qiin2333/Sunshine/blob/master/LICENSE',
    icon: 'fas fa-file-alt',
    titleKey: 'resource_card.license',
    descriptionKey: 'resource_card.view_license',
    variant: 'accent',
  },
  {
    id: 'third-party-notice',
    href: 'https://github.com/qiin2333/Sunshine/blob/master/NOTICE',
    icon: 'fas fa-exclamation-triangle',
    titleKey: 'resource_card.third_party_notice',
    descriptionKey: 'resource_card.third_party_desc',
    variant: 'accent',
  },
]

export const FEATURED_RESOURCES = [
  {
    id: 'alkaidlab',
    href: 'https://www.alkaidlab.com/',
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
