import { getReleaseDownloadUrls, resolveReleaseDownloadUrls } from '../utils/clientReleaseDownloads.js'

const ANDROID_GITHUB_REPOSITORY = 'qiin2333/moonlight-vplus'

const voidLinkStoreDownloads = (locale) => ({
  primary: locale === 'zh'
    ? 'https://apps.apple.com/cn/app/voidlink/id6747717070'
    : 'https://apps.apple.com/us/app/voidlink-extreme/id6755103808',
})

export const CLIENT_FEATURE_ICONS = {
  android: ['sports_esports', 'sensors', 'light_mode', 'mic'],
  windows: ['light_mode', 'content_paste', 'mic', 'folder_open'],
  macos: ['light_mode', 'sports_esports', 'content_paste', 'folder_open'],
  // one icon per feature of platforms.macos-enhanced.features (i18n/downloads/*.json), in the same order. The first six are the tags the V+ desktop clients show (same words, same icons) so nobody reads the
  // page and thinks V+ has something Enhanced has not; then two that Enhanced does best on macOS. Only what the PUBLISHED release (v1.3.9-rc.7, the one this page offers) has: each is in its README, release notes or code at that tag.
  'macos-enhanced': ['light_mode', 'sports_esports', 'content_paste', 'mic', 'folder_open', 'tune', 'mobile_vibrate', 'keyboard'],
  linux: ['light_mode', 'sports_esports', 'tune', 'bar_chart'],
  harmony: ['sports_esports', 'sensors', 'light_mode', 'graphic_eq'],
  ios: ['sports_esports', 'sensors', 'light_mode', 'desktop_mac'],
  'android-tv': ['sports_esports', 'light_mode', 'tune', 'bar_chart'],
  steam: ['sports_esports', 'link', 'bar_chart'],
  quest: ['desktop_windows', 'tune', 'bar_chart'],
  'apple-tv': ['sports_esports', 'light_mode', 'tv'],
}

// Tags of Moonlight macOS Enhanced that are NOT shown: the code is on the branch feat/enhanced-full (github.com/skyhua0224/moonlight-macos-enhanced) but not in the published v1.3.9-rc.7
// (checked with `git grep` on the tag: none of these names exists there). Nothing reads this list; when a release has the feature, move the entry to platforms.macos-enhanced.features and
// captions in i18n/downloads/*.json (and its icon to CLIENT_FEATURE_ICONS), and take it out here. `evidence` is where it is on the branch.
export const CLIENT_PENDING_TAGS = {
  'macos-enhanced': [
    { id: 'ds-player-leds', icon: 'lightbulb', en: 'DualSense player LEDs', zh: 'DualSense 玩家指示灯', evidence: 'moonlight-common-c src/Ds5ControlExtension.h DS5EXT_OUT_PLAYER_LEDS; Limelight/Input/DS5OutputMapping.h' },
    { id: 'ds-speaker-mic', icon: 'speaker', en: 'DualSense speaker and mic', zh: 'DualSense 扬声器与麦克风', evidence: 'Ds5ControlExtension.h DS5EXT_OUT_SPEAKER_VOLUME, MIC_VOLUME, MIC_MUTE, pad-speaker offer' },
    { id: 'ds-bluetooth-haptics', icon: 'mobile_vibrate', en: 'DualSense Bluetooth haptics', zh: 'DualSense 蓝牙触觉', evidence: 'Limelight/Input/DS5BluetoothPolicy.h (haptics.bt.fallback: IR, rumble, off)' },
    { id: 'ds-lightbar-brightness', icon: 'brightness_medium', en: 'DualSense lightbar brightness', zh: 'DualSense 灯条亮度', evidence: 'Ds5ControlExtension.h DS5EXT_OUT_LIGHTBAR_BRIGHTNESS' },
  ],
}

export const CLIENT_DEVICE_GROUPS = [
  { id: 'phone', icon: 'mobile', platforms: ['android', 'harmony', 'ios'] },
  { id: 'computer', icon: 'desktop_windows', platforms: ['windows', 'macos', 'linux'] },
  { id: 'tv', icon: 'tv', platforms: ['android-tv', 'apple-tv'] },
  { id: 'other', icon: 'sports_esports', platforms: ['steam', 'quest'] },
]

const downloadUrls = (result) => ({ primary: result.official, alternate: result.mirror })
const releaseDownloads = (product, device) => ({
  getDownloadUrls: (locale) => downloadUrls(getReleaseDownloadUrls(product, device, null, locale)),
  resolveDownloadUrls: async (locale) => downloadUrls(await resolveReleaseDownloadUrls(product, device, locale)),
})

export const CLIENT_PLATFORMS = [
  {
    id: 'android',
    icon: 'android',
    ...releaseDownloads('androidVplus', 'android'),
    project: `https://github.com/${ANDROID_GITHUB_REPOSITORY}`,
    release: `https://github.com/${ANDROID_GITHUB_REPOSITORY}/releases`,
  },
  {
    id: 'windows',
    icon: 'desktop_windows',
    ...releaseDownloads('moonlightPc', 'windows'),
    project: 'https://github.com/qiin2333/moonlight-qt',
    release: 'https://github.com/qiin2333/moonlight-qt/releases',
  },
  {
    id: 'harmony',
    kind: 'store',
    icon: 'mobile',
    getDownloadUrls: () => ({
      primary: 'https://appgallery.huawei.com/app/detail?id=com.alkaidlab.sdream',
    }),
    project: 'https://github.com/AlkaidLab/moonlight-harmony',
    release: 'https://appgallery.huawei.com/app/detail?id=com.alkaidlab.sdream',
  },
  {
    id: 'ios',
    kind: 'store',
    friendLink: true,
    icon: 'mobile',
    getDownloadUrls: voidLinkStoreDownloads,
    // the project page and both stores (the sheet's button opens the one of the interface language)
    links: [
      { id: 'voidlink-project', title: 'VoidLink', detail: 'GitHub The-Fried-Fish/VoidLink-previously-moonlight-zwm', icon: 'code', url: 'https://github.com/The-Fried-Fish/VoidLink-previously-moonlight-zwm#readme' },
      { id: 'store-cn', title: 'App Store', detail: 'CN · KR · VN', icon: 'download', url: 'https://apps.apple.com/app/voidlink/id6747717070' },
      { id: 'store-global', title: 'App Store', detail: 'Global', icon: 'download', url: 'https://apps.apple.com/app/voidlink-extreme/id6755103808' },
    ],
  },
  {
    id: 'macos',
    edition: 'standard',
    isNew: true,
    platformNew: true,
    icon: 'laptop_mac',
    ...releaseDownloads('moonlightPc', 'macos'),
    project: 'https://github.com/qiin2333/moonlight-qt',
    release: 'https://github.com/qiin2333/moonlight-qt/releases',
    alternatives: [{
      id: 'macos-enhanced',
      recommended: true,
      isNew: true,
      icon: 'laptop_mac',
      ...releaseDownloads('macos-enhanced', 'macos'),
      project: 'https://github.com/skyhua0224/moonlight-macos-enhanced',
      release: 'https://github.com/skyhua0224/moonlight-macos-enhanced/releases',
    }],
  },
  {
    id: 'linux',
    icon: 'computer',
    ...releaseDownloads('moonlightPc', 'linux'),
    project: 'https://github.com/qiin2333/moonlight-qt',
    release: 'https://github.com/qiin2333/moonlight-qt/releases',
  },
  {
    id: 'steam',
    icon: 'sports_esports',
    ...releaseDownloads('moonlightPc', 'steam'),
    project: 'https://github.com/qiin2333/moonlight-qt',
    release: 'https://github.com/qiin2333/moonlight-qt/releases',
  },
  {
    id: 'android-tv',
    icon: 'tv',
    ...releaseDownloads('androidVplus', 'android'),
    project: `https://github.com/${ANDROID_GITHUB_REPOSITORY}`,
    release: `https://github.com/${ANDROID_GITHUB_REPOSITORY}/releases`,
  },
  {
    id: 'quest',
    icon: 'view_in_ar',
    ...releaseDownloads('androidVplus', 'android'),
    project: `https://github.com/${ANDROID_GITHUB_REPOSITORY}`,
    release: `https://github.com/${ANDROID_GITHUB_REPOSITORY}/releases`,
  },
  {
    id: 'apple-tv',
    kind: 'store',
    isNew: true,
    friendLink: true,
    icon: 'tv',
    getDownloadUrls: voidLinkStoreDownloads,
    links: [{ id: 'testflight', title: 'TestFlight', detail: 'VoidLink beta · Apple TV', icon: 'science', url: 'https://testflight.apple.com/join/k7gs61mZ' }],
  },
]
