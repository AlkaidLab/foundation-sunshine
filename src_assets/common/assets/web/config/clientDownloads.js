import { getReleaseDownloadUrls, resolveReleaseDownloadUrls } from '../utils/clientReleaseDownloads.js'

const ANDROID_GITHUB_REPOSITORY = 'qiin2333/moonlight-vplus'

const voidLinkStoreDownloads = (locale) => ({
  primary: locale === 'zh'
    ? 'https://apps.apple.com/cn/app/voidlink/id6747717070'
    : 'https://apps.apple.com/us/app/voidlink-extreme/id6755103808',
})

export const CLIENT_FEATURE_ICONS = {
  android: ['fa-gamepad','fa-compass','fa-sun','fa-microphone'],
  windows: ['fa-sun','fa-clipboard','fa-microphone','fa-folder-open'],
  macos: ['fa-sun','fa-gamepad','fa-clipboard','fa-folder-open'],
  'macos-enhanced': ['fa-sun','fa-keyboard','fa-headphones','fa-sliders'],
  linux: ['fa-sun','fa-gamepad','fa-sliders','fa-chart-line'],
  harmony: ['fa-gamepad','fa-compass','fa-sun','fa-wave-square'],
  ios: ['fa-gamepad','fa-compass','fa-sun','fa-display'],
  'android-tv': ['fa-gamepad','fa-sun','fa-sliders','fa-chart-line'],
  steam: ['fa-gamepad','fa-link','fa-chart-line'],
  quest: ['fa-display','fa-sliders','fa-chart-line'],
  'apple-tv': ['fa-gamepad','fa-sun','fa-tv'],
}

export const CLIENT_DEVICE_GROUPS = [
  { id: 'phone', icon: 'fa-mobile-screen-button', platforms: ['android', 'harmony', 'ios'] },
  { id: 'computer', icon: 'fa-desktop', platforms: ['windows', 'macos', 'linux'] },
  { id: 'tv', icon: 'fa-tv', platforms: ['android-tv', 'apple-tv'] },
  { id: 'other', icon: 'fa-gamepad', platforms: ['steam', 'quest'] },
]

const downloadUrls = (result) => ({ primary: result.official, alternate: result.mirror })
const releaseDownloads = (product, device) => ({
  getDownloadUrls: (locale) => downloadUrls(getReleaseDownloadUrls(product, device, null, locale)),
  resolveDownloadUrls: async (locale) => downloadUrls(await resolveReleaseDownloadUrls(product, device, locale)),
})

export const CLIENT_PLATFORMS = [
  {
    id: 'android',
    icon: 'fa-android',
    ...releaseDownloads('androidVplus', 'android'),
    project: `https://github.com/${ANDROID_GITHUB_REPOSITORY}`,
    release: `https://github.com/${ANDROID_GITHUB_REPOSITORY}/releases`,
  },
  {
    id: 'windows',
    icon: 'fa-windows',
    ...releaseDownloads('moonlightPc', 'windows'),
    project: 'https://github.com/qiin2333/moonlight-qt',
    release: 'https://github.com/qiin2333/moonlight-qt/releases',
  },
  {
    id: 'harmony',
    kind: 'store',
    icon: 'fa-mobile-screen-button',
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
    icon: 'fa-apple',
    getDownloadUrls: voidLinkStoreDownloads,
  },
  {
    id: 'macos',
    edition: 'standard',
    isNew: true,
    platformNew: true,
    icon: 'fa-apple',
    ...releaseDownloads('moonlightPc', 'macos'),
    project: 'https://github.com/qiin2333/moonlight-qt',
    release: 'https://github.com/qiin2333/moonlight-qt/releases',
    alternatives: [{
      id: 'macos-enhanced',
      recommended: true,
      isNew: true,
      icon: 'fa-apple',
      ...releaseDownloads('macos-enhanced', 'macos'),
      project: 'https://github.com/skyhua0224/moonlight-macos-enhanced',
      release: 'https://github.com/skyhua0224/moonlight-macos-enhanced/releases',
    }],
  },
  {
    id: 'linux',
    icon: 'fa-linux',
    ...releaseDownloads('moonlightPc', 'linux'),
    project: 'https://github.com/qiin2333/moonlight-qt',
    release: 'https://github.com/qiin2333/moonlight-qt/releases',
  },
  {
    id: 'steam',
    icon: 'fa-steam',
    ...releaseDownloads('moonlightPc', 'steam'),
    project: 'https://github.com/qiin2333/moonlight-qt',
    release: 'https://github.com/qiin2333/moonlight-qt/releases',
  },
  {
    id: 'android-tv',
    icon: 'fa-tv',
    ...releaseDownloads('androidVplus', 'android'),
    project: `https://github.com/${ANDROID_GITHUB_REPOSITORY}`,
    release: `https://github.com/${ANDROID_GITHUB_REPOSITORY}/releases`,
  },
  {
    id: 'quest',
    icon: 'fa-vr-cardboard',
    ...releaseDownloads('androidVplus', 'android'),
    project: `https://github.com/${ANDROID_GITHUB_REPOSITORY}`,
    release: `https://github.com/${ANDROID_GITHUB_REPOSITORY}/releases`,
  },
  {
    id: 'apple-tv',
    kind: 'store',
    isNew: true,
    friendLink: true,
    icon: 'fa-tv',
    getDownloadUrls: voidLinkStoreDownloads,
  },
]
