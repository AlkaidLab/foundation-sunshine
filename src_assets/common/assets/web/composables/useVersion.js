import { ref, computed } from 'vue'
import { marked } from 'marked'
import SunshineVersion from '../sunshine_version.js'
import { trackEvents } from '../config/firebase.js'

const GITHUB_API_BASE = 'https://api.github.com/repos/qiin2333/Sunshine/releases'

/**
 * 解析 Markdown 内容
 */
const parseMarkdown = (text) => {
  if (!text) return ''
  return marked(text.replace(/\r\n/g, '\n').replace(/\r/g, '\n'), { breaks: true, gfm: true })
}

/**
 * 安全获取 GitHub 数据
 */
const fetchGitHub = async (url) => {
  try {
    const response = await fetch(url)
    return await response.json()
  } catch (e) {
    console.error(`Failed to fetch ${url}:`, e)
    return null
  }
}

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
  const installedVersionNotStable = computed(() => 
    githubVersion.value && version.value && version.value.isGreater(githubVersion.value)
  )

  const stableBuildAvailable = computed(() => 
    githubVersion.value && version.value && githubVersion.value.isGreater(version.value)
  )

  const preReleaseBuildAvailable = computed(() => 
    preReleaseVersion.value && version.value && preReleaseVersion.value.isGreater(version.value)
  )

  const buildVersionIsDirty = computed(() => {
    const v = version.value?.version
    return v?.split('.').length === 5 && v.includes('dirty')
  })

  const parsedStableBody = computed(() => parseMarkdown(githubVersion.value?.release?.body))
  const parsedPreReleaseBody = computed(() => parseMarkdown(preReleaseVersion.value?.release?.body))

  // 获取版本信息
  const fetchVersions = async (config) => {
    loading.value = true
    try {
      notifyPreReleases.value = config.notify_pre_releases || false
      version.value = new SunshineVersion(null, config.version)
      console.log('Version:', version.value.version)

      // 并行获取 GitHub 版本信息
      const [latestData, releases] = await Promise.all([
        fetchGitHub(`${GITHUB_API_BASE}/latest`),
        fetchGitHub(GITHUB_API_BASE)
      ])

      if (latestData) {
        githubVersion.value = new SunshineVersion(latestData, null)
        console.log('GitHub Version:', githubVersion.value.version)
      }

      const preRelease = Array.isArray(releases) && releases.find((r) => r.prerelease)
      if (preRelease) {
        preReleaseVersion.value = new SunshineVersion(preRelease, null)
        console.log('Pre-Release Version:', preReleaseVersion.value.version)
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
