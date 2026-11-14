import { computed, ref } from 'vue'
import { useVersion, VERSION_CHECK_STATUS } from './useVersion.js'
import { apiJson } from '../utils/apiFetch.js'

// One update check for the whole page, shared by the about page and the badge in the rail. It runs the same check as the home page
// (useVersion: the host's own version against the GitHub releases); it never installs anything.
let shared = null

const build = () => {
  const v = useVersion()
  let config = null
  // when a check last finished: the page's "updated" stamp
  const checkedAt = ref(0)
  let inflight = null

  const run = async (fresh) => {
    if (!config || fresh) config = await apiJson('/api/config')
    await v.fetchVersions(config)
    if (v.versionCheckStatus.value === VERSION_CHECK_STATUS.READY) checkedAt.value = Date.now()
  }
  // A check that is already running is joined; `force` asks the host for its configuration again (the pre-release choice may have changed).
  const check = (force = true) => (inflight ||= run(force).finally(() => { inflight = null }))
  // The quiet check of the rail badge: once per page load
  let ensured = false
  const ensure = () => { if (ensured) return inflight || Promise.resolve(); ensured = true; return check(false) }

  const available = computed(() => v.stableBuildAvailable.value || (v.notifyPreReleases.value && v.preReleaseBuildAvailable.value))
  const state = computed(() => {
    if (v.versionCheckStatus.value === VERSION_CHECK_STATUS.CHECKING) return 'checking'
    if (v.versionCheckStatus.value === VERSION_CHECK_STATUS.ERROR) return 'error'
    if (v.versionCheckStatus.value === VERSION_CHECK_STATUS.READY) return available.value ? 'available' : 'latest'
    return 'idle'
  })
  // the version a person would install: the newest of the offered ones
  const nextVersion = computed(() => (v.notifyPreReleases.value && v.preReleaseBuildAvailable.value ? v.preReleaseVersion.value?.version : v.stableBuildAvailable.value ? v.githubVersion.value?.version : ''))
  // the channel the installed build belongs to, as far as the releases tell: a build newer than the stable release is a pre-release
  const channel = computed(() => {
    const installed = v.version.value?.version
    if (!installed || v.versionCheckStatus.value !== VERSION_CHECK_STATUS.READY) return ''
    if (v.installedVersionNotStable.value) return 'prerelease'
    return v.githubVersion.value?.isEqual(v.version.value) ? 'stable' : ''
  })
  // 更新通道 of 关于: the host's own setting (notify_pre_releases, the switch in 配置 > 常规); saved the way the setup guide saves one key (the
  // whole configuration back), then the check runs again with the new choice. Resolves false (and shows the old choice) when it was not saved.
  const setPreReleases = async (on) => {
    const before = v.notifyPreReleases.value
    v.notifyPreReleases.value = on
    try {
      const current = await apiJson('/api/config')
      current.notify_pre_releases = on
      const result = await apiJson('/api/config', { method: 'POST', body: current })
      if (!(result?.status === true || result?.status === 'true')) throw new Error(result?.error || 'not saved')
      await check(true)
      return true
    } catch (error) {
      console.error('Saving the update channel failed:', error)
      v.notifyPreReleases.value = before
      return false
    }
  }
  return { ...v, check, ensure, state, available, nextVersion, channel, checkedAt, setPreReleases }
}

export const useUpdateStatus = () => (shared ||= build())
/** Tests only: drop the shared instance. */
export const resetUpdateStatus = () => { shared = null }
