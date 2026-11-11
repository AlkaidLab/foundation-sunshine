import { ref, computed } from 'vue'
import { marked } from 'marked'
import SunshineVersion from '../sunshine_version.js'
import { trackEvents } from '../config/telemetry.js'
import { apiJson } from '../utils/apiFetch.js'

const GITHUB_API_BASE = 'https://api.github.com/repos/qiin2333/Sunshine/releases'

export const VERSION_CHECK_STATUS = Object.freeze({
  IDLE: 'idle',
  CHECKING: 'checking',
  READY: 'ready',
  ERROR: 'error',
})

/**
 * 解析 Markdown 内容
 */
const parseMarkdown = (text) => {
  if (!text) return ''
  const normalized = text.replace(/\r\n?/g, '\n')
  return marked(normalized, { breaks: true, gfm: true })
}

/**
 * 安全获取 GitHub 数据
 */
const fetchGitHub = async (url) => {
  const response = await fetch(url)
  if (!response.ok) {
    throw new Error(`GitHub release request failed with status ${response.status}`)
  }
  return response.json()
}

/**
 * 将配置值转换为布尔值
 */
const toBoolean = (value) => {
  if (typeof value === 'string') {
    return value.toLowerCase() === 'true'
  }
  return Boolean(value)
}

/**
 * 版本管理组合式函数
 */
export function useVersion() {
  const version = ref(null)
  const githubVersion = ref(null)
  const preReleaseVersion = ref(null)
  const notifyPreReleases = ref(false)
  // the build record of GET /api/version; `dirty` there is the only thing that says the tree had uncommitted changes (the version string never does:
  // ".杂鱼" is on every CI release). A host without the endpoint leaves it null.
  const buildInfo = ref(null)
  const versionCheckStatus = ref(VERSION_CHECK_STATUS.IDLE)
  const loading = computed(() => versionCheckStatus.value === VERSION_CHECK_STATUS.CHECKING)

  // 计算属性
  // the installed build is newer than the newest stable release: a pre-release or a local build (versions compare by their date part only)
  const installedVersionNotStable = computed(() =>
    !!version.value && (githubVersion.value?.isLessThan(version.value) ?? false)
  )

  const stableBuildAvailable = computed(() =>
    githubVersion.value?.isGreater?.(version.value) ?? false
  )

  const preReleaseBuildAvailable = computed(() =>
    preReleaseVersion.value?.isGreater?.(version.value) ?? false
  )

  const buildVersionIsDirty = computed(() => buildInfo.value?.dirty === true)

  const parsedStableBody = computed(() => 
    parseMarkdown(githubVersion.value?.release?.body)
  )
  
  const parsedPreReleaseBody = computed(() => 
    parseMarkdown(preReleaseVersion.value?.release?.body)
  )

  /**
   * 获取版本信息
   */
  const fetchVersions = async (config) => {
    versionCheckStatus.value = VERSION_CHECK_STATUS.CHECKING
    githubVersion.value = null
    preReleaseVersion.value = null
    
    try {
      notifyPreReleases.value = toBoolean(config.notify_pre_releases)
      version.value = new SunshineVersion(null, config.version)
      // the build record is a nicety: an older host without it, or a failed read, never fails the version check
      try {
        const record = await apiJson('/api/version')
        buildInfo.value = record && typeof record === 'object' && !Array.isArray(record) ? record : null
      } catch {
        buildInfo.value = null
      }
      
      // 稳定版检查始终执行，预发布列表仅在用户订阅时请求
      const latestData = await fetchGitHub(`${GITHUB_API_BASE}/latest`)
      githubVersion.value = new SunshineVersion(latestData, null)

      if (notifyPreReleases.value) {
        const releases = await fetchGitHub(GITHUB_API_BASE)
        const preRelease = releases.find((r) => r.prerelease)
        if (preRelease) {
          preReleaseVersion.value = new SunshineVersion(preRelease, null)
        }
      }

      // 记录版本检查事件
      if (githubVersion.value && version.value) {
        trackEvents.versionChecked(version.value.version, githubVersion.value.version)
      }
      versionCheckStatus.value = VERSION_CHECK_STATUS.READY
    } catch (e) {
      versionCheckStatus.value = VERSION_CHECK_STATUS.ERROR
      console.error('Version check failed:', e)
      trackEvents.errorOccurred('version_check', e.message)
    }
  }

  return {
    version,
    githubVersion,
    preReleaseVersion,
    notifyPreReleases,
    buildInfo,
    versionCheckStatus,
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
