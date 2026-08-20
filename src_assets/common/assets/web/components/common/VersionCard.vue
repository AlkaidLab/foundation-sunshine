<template>
  <div id="version-details" class="card shadow-sm mb-4" v-if="version">
    <div class="card-header version-card-header">
      <h5 class="card-title mb-0">
        <i class="fas fa-code-branch me-2"></i>
        Version {{ version.version }}
      </h5>
    </div>
    <div class="card-body">
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

      <!-- 预发布版本可用 -->
      <div v-if="notifyPreReleases && preReleaseBuildAvailable" class="version-update">
        <div class="version-update-header">
          <div class="version-update-title">
            <i class="fas fa-rocket text-warning me-2"></i>
            <span>{{ $t('index.new_pre_release') }}</span>
          </div>
          <button
            type="button"
            class="btn btn-primary btn-download"
            :disabled="pendingNativeChannel !== ''"
            :aria-busy="pendingNativeChannel === 'prerelease'"
            @click="handleDownloadClick(preReleaseVersion.release.html_url, 'prerelease')"
          >
            <i :class="pendingNativeChannel === 'prerelease' ? 'fas fa-spinner fa-spin me-2' : 'fas fa-download me-2'"></i>
            {{ $t('index.download') }}
            <span v-if="nativeUpdaterAvailable" class="native-updater-badge">Control Panel</span>
          </button>
        </div>
        <h3 class="version-release-name">{{ preReleaseVersion.release.name }}</h3>
        <div class="markdown-content" v-html="parsedPreReleaseBody"></div>
      </div>

      <!-- 稳定版本可用 -->
      <div v-if="stableBuildAvailable" class="version-update">
        <div class="version-update-header">
          <div class="version-update-title">
            <i class="fas fa-star text-warning me-2"></i>
            <span>{{ $t('index.new_stable') }}</span>
          </div>
          <button
            type="button"
            class="btn btn-primary btn-download"
            :disabled="pendingNativeChannel !== ''"
            :aria-busy="pendingNativeChannel === 'stable'"
            @click="handleDownloadClick(githubVersion.release.html_url, 'stable')"
          >
            <i :class="pendingNativeChannel === 'stable' ? 'fas fa-spinner fa-spin me-2' : 'fas fa-download me-2'"></i>
            {{ $t('index.download') }}
            <span v-if="nativeUpdaterAvailable" class="native-updater-badge">Control Panel</span>
          </button>
        </div>
        <h3 class="version-release-name">{{ githubVersion.release.name }}</h3>
        <div class="markdown-content" v-html="parsedStableBody"></div>
      </div>
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
import { onBeforeUnmount, onMounted, ref } from 'vue'
import ConfirmDialog from './ConfirmDialog.vue'
import { openExternalUrl } from '../../utils/helpers.js'

defineProps({
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

  pendingDownloadUrl.value = url
  showDownloadConfirm.value = true
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
/* Loading State */
.version-loading {
  display: flex;
  align-items: center;
  padding: 1rem;
  color: var(--ui-text-secondary);
  font-size: 0.95rem;
}

/* Version Alerts */
.version-alert {
  border-radius: var(--ui-radius-sm);
  font-size: 0.9rem;
  padding: 0.75rem 1rem;
  margin-bottom: 1rem;
  display: flex;
  align-items: center;
  border: 1px solid transparent;
}

.version-alert-success {
  background: color-mix(in srgb, var(--ui-success) 12%, transparent);
  color: var(--ui-success-text);
  border-color: color-mix(in srgb, var(--ui-success) 30%, transparent);
  border-left: 4px solid var(--ui-success);
}

.version-alert-info {
  background: var(--ui-accent-soft);
  color: var(--ui-accent);
  border-color: var(--ui-border);
  border-left: 4px solid var(--ui-accent);
}

.version-card-header .card-title i {
  color: var(--ui-accent);
}

/* Version Update Section */
.version-update {
  background: color-mix(in srgb, var(--ui-warning) 10%, var(--ui-surface));
  border: 1px solid color-mix(in srgb, var(--ui-warning) 30%, transparent);
  border-radius: var(--ui-radius-md);
  padding: 1.25rem;
  margin-top: 1rem;
}

.version-update-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  flex-wrap: wrap;
  gap: 1rem;
  margin-bottom: 1rem;
}

.version-update-title {
  display: flex;
  align-items: center;
  font-size: 1rem;
  font-weight: 600;
  color: var(--ui-text-primary);
  flex: 1;
  min-width: 200px;
}

.btn-download {
  border-radius: 8px;
  padding: 0.5rem 1.25rem;
  font-weight: 600;
  transition: transform 0.2s ease, box-shadow 0.2s ease;
  white-space: nowrap;
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
  font-size: 1.3rem;
  font-weight: 600;
  margin: 1rem 0 0.75rem 0;
  color: var(--ui-text-primary);
}

/* Markdown Content */
.markdown-content {
  background: var(--ui-surface);
  border-radius: var(--ui-radius-sm);
  padding: 1.25rem;
  margin-top: 1rem;
  line-height: 1.6;
  border: 1px solid var(--ui-border);
}

.markdown-content h1,
.markdown-content h2,
.markdown-content h3,
.markdown-content h4,
.markdown-content h5,
.markdown-content h6 {
  margin-top: 1.25rem;
  margin-bottom: 0.75rem;
  font-weight: 600;
  line-height: 1.25;
  color: var(--ui-text-primary);
}

.markdown-content h1:first-child,
.markdown-content h2:first-child,
.markdown-content h3:first-child {
  margin-top: 0;
}

.markdown-content h1 {
  font-size: 1.5em;
}

.markdown-content h2 {
  font-size: 1.3em;
}

.markdown-content h3 {
  font-size: 1.1em;
}

.markdown-content p {
  margin-bottom: 0.75rem;
  white-space: pre-line;
  color: var(--ui-text-secondary);
}

.markdown-content ul,
.markdown-content ol {
  margin-bottom: 0.75rem;
  padding-left: 1.5rem;
}

.markdown-content li {
  margin-bottom: 0.5rem;
  color: var(--ui-text-secondary);
}

.markdown-content code {
  background: var(--ui-accent-soft);
  padding: 0.2em 0.4em;
  border-radius: 4px;
  font-family: 'Courier New', 'Consolas', 'Monaco', monospace;
  font-size: 0.9em;
  color: var(--ui-accent);
}

.markdown-content pre {
  background: var(--ui-surface-strong);
  padding: 1rem;
  border-radius: 8px;
  overflow-x: auto;
  margin: 1rem 0;
  border: 1px solid var(--ui-border);
}

.markdown-content pre code {
  background: none;
  padding: 0;
  color: inherit;
}

.markdown-content blockquote {
  border-left: 4px solid var(--ui-accent);
  margin: 1rem 0;
  padding-left: 1rem;
  color: var(--ui-text-secondary);
  font-style: italic;
}

.markdown-content a {
  color: var(--ui-accent);
  text-decoration: none;
  font-weight: 500;
  transition: color 0.2s ease;
}

.markdown-content a:hover {
  color: var(--ui-accent);
  text-decoration: underline;
}

.markdown-content table {
  border-collapse: collapse;
  width: 100%;
  margin: 1rem 0;
  border-radius: 8px;
  overflow: hidden;
}

.markdown-content th,
.markdown-content td {
  border: 1px solid var(--ui-border);
  padding: 0.75rem 1rem;
  text-align: left;
}

.markdown-content th {
  background: var(--ui-surface-strong);
  font-weight: 600;
}

</style>
