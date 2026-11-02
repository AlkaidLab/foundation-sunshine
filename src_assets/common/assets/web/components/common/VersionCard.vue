<template>
  <div id="version-details" class="version-card" v-if="version">
    <div class="version-card-body">
      <!-- 加载状态 -->
      <div v-if="loading" class="version-loading">
        <i class="fas fa-spinner fa-spin me-2"></i>
        {{ $t('index.loading_latest') }}
      </div>

      <!-- 开发版本标识 -->
      <div class="version-alert version-alert-success" v-if="buildVersionIsDirty">
        <i class="fas fa-code me-2"></i>
        {{ $t('index.version_dirty') }} 🌇
      </div>

      <!-- 已安装版本不是稳定版 -->
      <div class="version-alert version-alert-info" v-if="installedVersionNotStable">
        <i class="fas fa-info-circle me-2"></i>
        {{ $t('index.installed_version_not_stable') }}
      </div>

      <!-- 已是最新版本 -->
      <div
        v-else-if="(!preReleaseBuildAvailable || !notifyPreReleases) && !stableBuildAvailable && !buildVersionIsDirty"
        class="version-alert version-alert-success"
      >
        <i class="fas fa-check-circle me-2"></i>
        {{ $t('index.version_latest') }}
      </div>

      <!-- 可用更新：用排版和分隔线组织内容，避免卡片层层嵌套 -->
      <section
        v-for="update in availableUpdates"
        :key="update.channel"
        class="version-update"
        :aria-labelledby="`version-update-${update.channel}`"
      >
        <div class="version-update-summary">
          <div class="version-update-copy">
            <span class="release-channel">{{ update.channelLabel }}</span>
            <h2 :id="`version-update-${update.channel}`" class="version-update-title">
              {{ $t('index.update_available') }}
            </h2>
            <h3 class="version-release-name">{{ update.release.name }}</h3>
            <p class="version-update-description">{{ $t(update.descriptionKey) }}</p>
          </div>
          <div class="version-update-actions">
            <button
              type="button"
              class="btn-download"
              :disabled="pendingNativeChannel !== '' || (!nativeUpdaterAvailable && !update.release.html_url)"
              :aria-busy="pendingNativeChannel === update.channel"
              @click="handleDownloadClick(update.release.html_url, update.channel)"
            >
              <i :class="pendingNativeChannel === update.channel ? 'fas fa-spinner fa-spin me-2' : 'fas fa-download me-2'"></i>
              {{ $t('index.download') }}
              <span v-if="nativeUpdaterAvailable" class="native-updater-badge">Control Panel</span>
            </button>
            <button
              v-if="update.release.html_url"
              type="button"
              class="release-page-link"
              @click="handleReleasePageClick(update.release.html_url)"
            >
              {{ $t('index.view_release') }}
              <i class="fas fa-arrow-up-right-from-square" aria-hidden="true"></i>
            </button>
          </div>
        </div>
        <div v-if="update.details.notes" class="version-notes">
          <h3 class="version-notes-title">{{ $t('index.update_details') }}</h3>
          <div class="markdown-content" v-html="update.details.notes"></div>
        </div>
        <div
          v-if="update.details.fullChangelog || update.details.contributors"
          class="version-update-footer"
        >
          <button
            v-if="update.details.fullChangelog"
            type="button"
            class="full-changelog"
            @click="handleReleasePageClick(update.details.fullChangelog)"
          >
            <i class="fas fa-arrow-up-right-from-square" aria-hidden="true"></i>
            <span>{{ $t('index.full_changelog') }}</span>
            <i class="fas fa-chevron-right" aria-hidden="true"></i>
          </button>
          <div v-if="update.details.contributors" class="release-contributors">
            <span class="release-footer-label">{{ $t('index.contributors') }}</span>
            <div class="contributors-content" v-html="update.details.contributors"></div>
          </div>
        </div>
      </section>
    </div>

    <!-- 下载确认弹窗（与配置页虚拟麦克风下载相同方式，确认后打开下载页） -->
    <ConfirmDialog
      :show="showDownloadConfirm"
      dialog-id="version-download-confirm"
      :title="$t('_common.download')"
      title-icon="fas fa-external-link-alt"
      :close-label="$t('_common.close')"
      @close="cancelDownload"
    >
      <p>{{ $t('index.update_download_confirm') }}</p>
      <template #actions>
        <button type="button" class="btn btn-secondary" @click="cancelDownload">{{ $t('_common.cancel') }}</button>
        <button type="button" class="btn btn-primary" @click="confirmDownload">
          <i class="fas fa-download me-1"></i>{{ $t('_common.download') }}
        </button>
      </template>
    </ConfirmDialog>
  </div>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import ConfirmDialog from './ConfirmDialog.vue'
import { openExternalUrl } from '../../utils/helpers.js'
import { extractReleaseDetails } from '../../utils/releaseDetails.js'

const props = defineProps({
  version: Object,
  githubVersion: Object,
  preReleaseVersion: Object,
  notifyPreReleases: Boolean,
  loading: Boolean,
  installedVersionNotStable: Boolean,
  stableBuildAvailable: Boolean,
  preReleaseBuildAvailable: Boolean,
  buildVersionIsDirty: Boolean,
  parsedStableBody: String,
  parsedPreReleaseBody: String,
})

const availableUpdates = computed(() => {
  const updates = []

  if (props.notifyPreReleases && props.preReleaseBuildAvailable && props.preReleaseVersion?.release) {
    updates.push({
      channel: 'prerelease',
      channelLabel: 'Beta',
      descriptionKey: 'index.new_pre_release',
      release: props.preReleaseVersion.release,
      details: extractReleaseDetails(props.parsedPreReleaseBody),
    })
  }

  if (props.stableBuildAvailable && props.githubVersion?.release) {
    updates.push({
      channel: 'stable',
      channelLabel: 'Stable',
      descriptionKey: 'index.new_stable',
      release: props.githubVersion.release,
      details: extractReleaseDetails(props.parsedStableBody),
    })
  }

  return updates
})

const showDownloadConfirm = ref(false)
const pendingDownloadUrl = ref('')
const nativeUpdaterAvailable = ref(false)
const pendingNativeChannel = ref('')
let pendingNativeRequestId = ''
let nativeRequestTimer = null
const contextRequestTimers = []

const CONTROL_PANEL_ORIGINS = new Set([
  'http://tauri.localhost',
  'https://tauri.localhost',
  'tauri://localhost',
  'http://localhost:8080',
  'https://localhost:8080',
])

const normalizeOrigin = (url) => {
  try {
    const parsed = new URL(url)
    return parsed.origin === 'null' ? `${parsed.protocol}//${parsed.host}` : parsed.origin
  } catch {
    return ''
  }
}

const controlPanelOrigin = [
  document.referrer,
  window.location.ancestorOrigins?.[0],
].map(normalizeOrigin).find((origin) => CONTROL_PANEL_ORIGINS.has(origin)) || ''

const requestNativeUpdaterContext = () => {
  if (window.parent === window || !CONTROL_PANEL_ORIGINS.has(controlPanelOrigin)) return
  window.parent.postMessage(
    {
      type: 'native-updater-context-request',
      source: 'sunshine-webui',
    },
    controlPanelOrigin
  )
}

const clearNativeRequest = () => {
  pendingNativeChannel.value = ''
  pendingNativeRequestId = ''
  if (nativeRequestTimer) {
    clearTimeout(nativeRequestTimer)
    nativeRequestTimer = null
  }
}

const handleNativeUpdaterMessage = (event) => {
  if (
    event.source !== window.parent
    || event.origin !== controlPanelOrigin
    || !CONTROL_PANEL_ORIGINS.has(event.origin)
    || event.data?.source !== 'sunshine-control-panel'
  ) return

  if (event.data.type === 'native-updater-context') {
    nativeUpdaterAvailable.value = event.data.available === true
    return
  }

  if (
    event.data.type === 'native-update-result'
    && event.data.requestId === pendingNativeRequestId
  ) {
    clearNativeRequest()
  }
}

const requestNativeUpdate = (channel) => {
  if (!CONTROL_PANEL_ORIGINS.has(controlPanelOrigin)) return
  pendingNativeChannel.value = channel
  pendingNativeRequestId = `${Date.now()}-${Math.random().toString(36).slice(2)}`
  window.parent.postMessage(
    {
      type: 'native-update-request',
      source: 'sunshine-webui',
      requestId: pendingNativeRequestId,
      channel,
    },
    controlPanelOrigin
  )

  nativeRequestTimer = setTimeout(clearNativeRequest, 30000)
}

const handleDownloadClick = (url, channel) => {
  if (nativeUpdaterAvailable.value) {
    requestNativeUpdate(channel)
    return
  }

  if (!url) return

  pendingDownloadUrl.value = url
  showDownloadConfirm.value = true
}

const handleReleasePageClick = async (url) => {
  if (!url) return
  try {
    await openExternalUrl(url)
  } catch (error) {
    console.error('Failed to open release URL:', error)
  }
}

const confirmDownload = async () => {
  const url = pendingDownloadUrl.value
  showDownloadConfirm.value = false
  pendingDownloadUrl.value = ''
  if (!url) return
  try {
    await openExternalUrl(url)
  } catch (error) {
    console.error('Failed to open download URL:', error)
  }
}

const cancelDownload = () => {
  showDownloadConfirm.value = false
  pendingDownloadUrl.value = ''
}

onMounted(() => {
  window.addEventListener('message', handleNativeUpdaterMessage)
  requestNativeUpdaterContext()
  contextRequestTimers.push(setTimeout(requestNativeUpdaterContext, 500))
  contextRequestTimers.push(setTimeout(requestNativeUpdaterContext, 1500))
})

onBeforeUnmount(() => {
  window.removeEventListener('message', handleNativeUpdaterMessage)
  contextRequestTimers.forEach(clearTimeout)
  clearNativeRequest()
})
</script>

<style scoped>
.version-card {
  /* Normalize the base size: the details wrapper on the home page renders at
     0.8rem, and the content inside should read like the rest of the page. */
  font-size: 14px;
}

.version-card-body {
  padding: 0;
}

/* Loading State */
.version-loading {
  display: flex;
  align-items: center;
  padding: 1.25rem 0 0;
  color: var(--home-muted, var(--ui-text-secondary));
  font-size: 14px;
}

/* Version Alerts */
.version-alert {
  margin: 1.4rem 0 0;
  display: flex;
  align-items: center;
  gap: 0.5rem;
  font-size: 13px;
  color: var(--home-body, var(--ui-text-secondary));
}

.version-alert-success {
  color: var(--ui-success-text);
}

.version-alert-info {
  color: var(--ui-accent-text);
}

/* Version Update Section */
.version-update {
  padding: 1.6rem 0 0;
}

.version-update + .version-update {
  margin-top: 1.6rem;
  border-top: 0;
}

.version-update-summary {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  align-items: center;
  gap: 1.5rem 2rem;
}

.version-update-title {
  margin: 0.7rem 0 0;
  color: var(--home-heading, var(--ui-text-primary));
  font-size: 20px;
  font-weight: 650;
  line-height: 1.4;
}

.release-channel {
  display: inline-flex;
  align-items: center;
  padding: 0;
  color: var(--ui-accent-text);
  font-size: 13px;
  font-weight: 700;
  letter-spacing: 0.06em;
}

.version-update-description {
  margin: 0.7rem 0 0;
  color: var(--home-muted, var(--ui-text-secondary));
  font-size: 14px;
  line-height: 1.55;
}

.version-update-actions {
  display: flex;
  min-width: 10rem;
  flex-direction: column;
  align-items: stretch;
  gap: 0.7rem;
}

.btn-download {
  min-height: 2.6rem;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  border: 0;
  border-radius: 2rem;
  padding: 0.6rem 1.35rem;
  background: var(--ui-accent);
  color: var(--ui-accent-contrast);
  font-weight: 700;
  white-space: nowrap;
  transition:
    transform 150ms ease,
    filter 150ms ease;
}

.native-updater-badge {
  display: inline-flex;
  align-items: center;
  margin-left: 0.55rem;
  padding: 0.12rem 0.4rem;
  border: 1px solid color-mix(in srgb, currentColor 45%, transparent);
  border-radius: 999px;
  font-size: 0.7rem;
  font-weight: 600;
  line-height: 1.2;
}

.btn-download:hover:not(:disabled) {
  transform: translateY(-1px);
  filter: brightness(1.06);
  color: var(--ui-accent-contrast);
}

.btn-download:disabled {
  opacity: 0.55;
  cursor: not-allowed;
}

.version-release-name {
  margin: 0.45rem 0 0;
  color: var(--home-heading, var(--ui-text-primary));
  font-family: 'Segoe UI Variable Display', 'Microsoft YaHei Light', 'Segoe UI', 'Microsoft YaHei',
    sans-serif;
  font-size: clamp(1.5rem, 2.5vw, 2rem);
  font-weight: 700;
  line-height: 1.3;
  letter-spacing: -0.01em;
  overflow-wrap: anywhere;
}

.release-page-link {
  align-self: center;
  padding: 0.1rem 0.35rem;
  border: 0;
  background: transparent;
  color: var(--home-body, var(--ui-text-secondary));
  font-size: 0.88rem;
  font-weight: 500;
}

.release-page-link i {
  margin-left: 0.3rem;
  font-size: 0.72rem;
}

.release-page-link:hover {
  color: var(--ui-accent-text);
  text-decoration: underline;
  text-underline-offset: 3px;
}

/* Release notes */
.version-notes {
  margin-top: 1.75rem;
}

.version-notes-title {
  margin: 0;
  color: var(--home-muted, var(--ui-text-secondary));
  font-size: 13px;
  font-weight: 600;
  letter-spacing: 0.08em;
}

.markdown-content {
  margin-top: 0.9rem;
  line-height: 1.6;
}

.native-updater-badge {
  display: inline-flex;
  align-items: center;
  margin-left: 0.55rem;
  padding: 0.12rem 0.4rem;
  border: 1px solid color-mix(in srgb, currentColor 45%, transparent);
  border-radius: 999px;
  font-size: 0.7rem;
  font-weight: 600;
  line-height: 1.2;
}

.btn-download:hover {
  transform: translateY(-1px);
  box-shadow: var(--ui-shadow-sm);
}

.version-release-name {
  margin: 0.45rem 0 0;
  color: var(--ui-accent);
  font-size: clamp(1.45rem, 2.4vw, 1.9rem);
  font-weight: 700;
  line-height: 1.25;
  overflow-wrap: anywhere;
}

.release-page-link {
  align-self: center;
  padding: 0.1rem 0.35rem;
  border: 0;
  background: transparent;
  color: var(--ui-accent);
  font-size: 0.88rem;
  font-weight: 600;
}

.release-page-link i {
  margin-left: 0.3rem;
  font-size: 0.72rem;
}

.release-page-link:hover {
  text-decoration: underline;
}

/* Release notes */
.version-notes {
  margin-top: 1.75rem;
}

.version-notes-title {
  margin: 0;
  color: var(--home-heading, var(--ui-text-primary));
  font-size: 1.16rem;
  font-weight: 700;
}

.markdown-content {
  margin-top: 0.9rem;
  line-height: 1.6;
}

.markdown-content :deep(h1),
.markdown-content :deep(h2),
.markdown-content :deep(h3),
.markdown-content :deep(h4),
.markdown-content :deep(h5),
.markdown-content :deep(h6) {
  margin-top: 1.4rem;
  margin-bottom: 0.75rem;
  font-weight: 600;
  line-height: 1.25;
  color: var(--home-heading, var(--ui-text-primary));
}

.markdown-content :deep(h1) {
  font-size: 1.35em;
}

.markdown-content :deep(h2) {
  font-size: 1.2em;
}

.markdown-content :deep(h3) {
  font-size: 1.1em;
}

.markdown-content :deep(p) {
  margin-bottom: 0.75rem;
  white-space: pre-line;
  color: var(--home-body, var(--ui-text-secondary));
}

.markdown-content :deep(ul),
.markdown-content :deep(ol) {
  margin-bottom: 0.75rem;
  padding-left: 1.5rem;
}

.markdown-content :deep(li) {
  margin-bottom: 0.42rem;
  color: var(--home-body, var(--ui-text-secondary));
}

.markdown-content :deep(li::marker) {
  color: var(--ui-accent);
}

.markdown-content :deep(code) {
  background: var(--ui-accent-soft);
  padding: 0.2em 0.4em;
  border-radius: 4px;
  font-family: 'Courier New', 'Consolas', 'Monaco', monospace;
  font-size: 0.9em;
  color: var(--ui-accent);
}

.markdown-content :deep(pre) {
  margin: 1rem 0;
  padding: 0.2rem 0 0.2rem 1rem;
  border-left: 2px solid color-mix(in srgb, var(--ui-accent) 40%, transparent);
  overflow-x: auto;
}

.markdown-content :deep(pre code) {
  background: none;
  padding: 0;
  color: inherit;
}

.markdown-content :deep(blockquote) {
  border-left: 4px solid var(--ui-accent);
  margin: 1rem 0;
  padding-left: 1rem;
  color: var(--home-body, var(--ui-text-secondary));
  font-style: italic;
}

.markdown-content :deep(a) {
  color: var(--ui-accent);
  text-decoration: none;
  font-weight: 500;
  transition: color 0.2s ease;
}

.markdown-content :deep(a:hover) {
  color: var(--ui-accent);
  text-decoration: underline;
}

.markdown-content :deep(table) {
  border-collapse: collapse;
  width: 100%;
  margin: 1rem 0;
  border-radius: 8px;
  overflow: hidden;
}

.markdown-content :deep(img) {
  border-radius: 50%;
}

.markdown-content :deep(th),
.markdown-content :deep(td) {
  border: 1px solid var(--ui-border);
  padding: 0.75rem 1rem;
  text-align: left;
}

.markdown-content :deep(th) {
  font-weight: 600;
}

.version-update-footer {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 1rem 2rem;
  margin-top: 1.35rem;
  padding-top: 0;
}

.full-changelog {
  display: inline-flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.25rem 0;
  border: 0;
  background: transparent;
  color: var(--ui-accent);
  font-size: 0.9rem;
  font-weight: 600;
}

.full-changelog:hover {
  text-decoration: underline;
}

.full-changelog .fa-chevron-right {
  font-size: 0.68rem;
}

.release-contributors {
  display: flex;
  align-items: center;
  gap: 0.8rem;
  margin-left: auto;
  color: var(--home-body, var(--ui-text-secondary));
}

.release-footer-label {
  font-size: 0.88rem;
  font-weight: 600;
  white-space: nowrap;
}

.contributors-content {
  display: flex;
  align-items: center;
}

.contributors-content :deep(p) {
  display: flex;
  align-items: center;
  gap: 0.45rem;
  margin: 0;
}

.contributors-content :deep(ul) {
  display: flex;
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: 0.35rem 0.75rem;
  margin: 0;
  padding: 0;
  list-style: none;
}

.contributors-content :deep(img) {
  width: 2.25rem;
  height: 2.25rem;
  border: 2px solid color-mix(in srgb, var(--ui-surface-strong) 90%, transparent);
  border-radius: 50%;
  object-fit: cover;
  box-shadow: var(--ui-shadow-sm);
}

.contributors-content :deep(a) {
  color: var(--ui-accent);
  text-decoration: none;
}

@media (max-width: 720px) {
  .version-card-body {
    padding: 0;
  }

  .version-update-summary {
    grid-template-columns: minmax(0, 1fr);
    gap: 1.25rem;
  }

  .version-update-actions {
    min-width: 0;
  }

  .btn-download {
    width: 100%;
  }

  .markdown-content {
    overflow-x: auto;
  }

  .version-update-footer {
    align-items: flex-start;
    flex-direction: column;
  }

  .release-contributors {
    align-self: flex-end;
  }
}

</style>
