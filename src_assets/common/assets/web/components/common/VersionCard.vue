<template>
  <MUpdateSheet
    v-if="version"
    id="version-details"
    :model-value="modelValue"
    :phase="phase"
    :current="phase === 'latest' ? '' : version.version"
    :next="phase === 'latest' ? version.version : sheet?.next || ''"
    :channel="phase === 'latest' ? '' : sheet?.channel || ''"
    :notes="phase === 'latest' ? null : sheet?.notes || null"
    :drivers="phase === 'latest' ? null : sheet?.drivers || null"
    :error-text="phase === 'error' ? tx.checkFailed : ''"
    :can-install="false"
    :link-note="nativeUpdaterAvailable ? 'via-panel' : ''"
    :pending="pendingNativeChannel !== ''"
    :can-skip="false"
    :has-release-page="!!sheet?.releasePage"
    :locale="locale"
    :close-label="$t('_common.close')"
    @update:model-value="(value) => emit('update:modelValue', value)"
    @update="handleDownloadClick(sheet?.releasePage, selected)"
    @retry="emit('retry')"
    @open-release="handleReleasePageClick(sheet?.releasePage)"
    @open-changelog="handleReleasePageClick"
  >
    <template #channels>
      <MSeg v-if="updates.length > 1" :model-value="selected" block :options="channelOptions" :label="$t('index.check_updates')" @update:model-value="pickChannel" />
    </template>
    <template #notice>
      <MBanner v-if="buildVersionIsDirty" tone="info" icon="code">{{ $t('index.version_dirty') }}</MBanner>
      <MBanner v-if="installedVersionNotStable" tone="info" icon="info">{{ $t('index.installed_version_not_stable') }}</MBanner>
    </template>
  </MUpdateSheet>

  <!-- Download confirmation: the same as the virtual microphone download in the configuration -->
  <ConfirmDialog
    :show="showDownloadConfirm"
    dialog-id="version-download-confirm"
    :title="$t('_common.download')"
    title-icon="open_in_new"
    :close-label="$t('_common.close')"
    @close="cancelDownload"
  >
    <p>{{ $t('index.update_download_confirm') }}</p>
    <template #actions>
      <MButton variant="text" @click="cancelDownload">{{ $t('_common.cancel') }}</MButton>
      <MButton icon="download" @click="confirmDownload">{{ $t('_common.download') }}</MButton>
    </template>
  </ConfirmDialog>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { MBanner, MButton, MSeg, MUpdateSheet, sheetFromGithubRelease, updateTexts } from 'foundation-ui/vue'
import ConfirmDialog from './ConfirmDialog.vue'
import { openExternalUrl } from '../../utils/helpers.js'

// The update sheet of the Web UI. This page cannot install anything itself: the primary action opens the release page
// (after a confirmation), or asks the Control Panel to update when the page runs inside it. The sheet says which one.
const props = defineProps({
  modelValue: { type: Boolean, default: false },
  version: Object,
  githubVersion: Object,
  preReleaseVersion: Object,
  notifyPreReleases: Boolean,
  loading: Boolean,
  checkFailed: Boolean,
  installedVersionNotStable: Boolean,
  stableBuildAvailable: Boolean,
  preReleaseBuildAvailable: Boolean,
  buildVersionIsDirty: Boolean,
})
const emit = defineEmits(['update:modelValue', 'retry'])
const { locale } = useI18n()
const tx = computed(() => updateTexts(locale.value))

// The releases offered, newest channel first: the pre-release (when subscribed) and the stable one.
const updates = computed(() => {
  const list = []
  if (props.notifyPreReleases && props.preReleaseBuildAvailable && props.preReleaseVersion?.release) {
    list.push({ channel: 'prerelease', release: props.preReleaseVersion.release })
  }
  if (props.stableBuildAvailable && props.githubVersion?.release) {
    list.push({ channel: 'stable', release: props.githubVersion.release })
  }
  return list
})
// The newest channel is shown first, until the user picks one (the pre-release list arrives after the stable release).
const selected = ref('')
let picked = false
const pickChannel = (value) => { picked = true; selected.value = value }
watch(updates, (list) => {
  if (!picked || !list.some((entry) => entry.channel === selected.value)) selected.value = list[0]?.channel || ''
}, { immediate: true })
const channelOptions = computed(() => updates.value.map((entry) => ({ value: entry.channel, label: entry.channel === 'prerelease' ? tx.value.channelPrerelease : tx.value.channelStable })))

const sheet = computed(() => {
  const entry = updates.value.find((item) => item.channel === selected.value)
  return entry ? sheetFromGithubRelease({ ...entry.release, prerelease: entry.channel === 'prerelease' }, { current: props.version?.version }) : null
})
const phase = computed(() => {
  if (props.loading) return 'checking'
  if (props.checkFailed) return 'error'
  return sheet.value ? 'available' : 'latest'
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
