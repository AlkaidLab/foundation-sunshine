import { ref, computed } from 'vue'
import { marked } from 'marked'
import SunshineVersion from '../sunshine_version.js'
import { trackEvents } from '../config/firebase.js'

/**
 * 版本管理组合式函数
 */
export function useVersion() {
  const version = ref(null)
  const githubVersion = ref(null)
  const preReleaseVersion = ref(null)
  const notifyPreReleases = ref(false)
  const loading = ref(true)

  // 计算属性
  const installedVersionNotStable = computed(() => {
    if (!githubVersion.value || !version.value) {
      return false
    }
    return version.value.isGreater(githubVersion.value)
  })

  const stableBuildAvailable = computed(() => {
    if (!githubVersion.value || !version.value) {
      return false
    }
    return githubVersion.value.isGreater(version.value)
  })

  const preReleaseBuildAvailable = computed(() => {
    if (!preReleaseVersion.value || !githubVersion.value || !version.value) {
      return false
    }
    return preReleaseVersion.value.isGreater(version.value)
  })

  const buildVersionIsDirty = computed(() => {
    return version.value?.version?.split('.').length === 5 && 
           version.value.version.indexOf('dirty') !== -1
  })

  const parsedStableBody = computed(() => {
    if (!githubVersion.value?.release?.body) return ''
    const processedText = githubVersion.value.release.body
      .replace(/\r\n/g, '\n')
      .replace(/\r/g, '\n')
    return marked(processedText, {
      breaks: true,
      gfm: true,
    })
  })

  const parsedPreReleaseBody = computed(() => {
    if (!preReleaseVersion.value?.release?.body) return ''
    const processedText = preReleaseVersion.value.release.body
      .replace(/\r\n/g, '\n')
      .replace(/\r/g, '\n')
    return marked(processedText, {
      breaks: true,
      gfm: true,
    })
  })

  // 获取版本信息
  const fetchVersions = async (config) => {
    loading.value = true
    try {
      notifyPreReleases.value = config.notify_pre_releases || false
      version.value = new SunshineVersion(null, config.version)
      console.log('Version: ', version.value.version)

      // 获取最新稳定版
      try {
        const latestResponse = await fetch('https://api.github.com/repos/qiin2333/Sunshine/releases/latest')
        const latestData = await latestResponse.json()
        githubVersion.value = new SunshineVersion(latestData, null)
        console.log('GitHub Version: ', githubVersion.value.version)
      } catch (e) {
        console.error('Failed to fetch latest release:', e)
      }

      // 获取预发布版
      try {
        const releasesResponse = await fetch('https://api.github.com/repos/qiin2333/Sunshine/releases')
        const releases = await releasesResponse.json()
        const preRelease = Array.isArray(releases) 
          ? releases.find((release) => release.prerelease) 
          : null
        if (preRelease) {
          preReleaseVersion.value = new SunshineVersion(preRelease, null)
          console.log('Pre-Release Version: ', preReleaseVersion.value.version)
        }
      } catch (e) {
        console.error('Failed to fetch pre-release:', e)
      }

      // 记录版本检查事件
      if (githubVersion.value && version.value) {
        trackEvents.versionChecked(version.value.version, githubVersion.value.version)
      }
    } catch (e) {
      console.error(e)
      trackEvents.errorOccurred('version_check', e.message)
    } finally {
      loading.value = false
    }
  }

  return {
    version,
    githubVersion,
    preReleaseVersion,
    notifyPreReleases,
    loading,
    installedVersionNotStable,
    stableBuildAvailable,
    preReleaseBuildAvailable,
    buildVersionIsDirty,
    parsedStableBody,
    parsedPreReleaseBody,
    fetchVersions,
  }
}

