<script setup>
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { $tp } from '../../../platform-i18n'
import { MItem, MSwitchItem } from 'foundation-ui/vue'

const props = defineProps({
  config: Object,
})

const config = ref(props.config)
const createStatus = (overrides = {}) => ({
  state: 'idle',
  artifacts_installed: false,
  display_available: false,
  ...overrides,
})
const status = ref(createStatus())
const statusLoaded = ref(false)
const validating = ref(false)
let statusTimer
let statusActive = false

const enabled = computed({
  get: () => config.value.vdd_vulkan_hdr_bridge === 'enabled',
  set: (value) => {
    config.value.vdd_vulkan_hdr_bridge = value ? 'enabled' : 'disabled'
  },
})

const viewState = computed(() => {
  if (!enabled.value) {
    return {
      statusKey: 'config.vdd_vulkan_hdr_bridge_status_disabled',
      tone: 'muted',
      canValidate: false,
      showValidateAction: false,
      showValidateHint: false,
    }
  }
  if (!statusLoaded.value) {
    return {
      statusKey: 'config.vdd_vulkan_hdr_bridge_status_loading',
      tone: 'muted',
      canValidate: false,
      showValidateAction: false,
      showValidateHint: false,
    }
  }

  const { state, artifacts_installed: artifactsInstalled, display_available: displayAvailable } =
    status.value
  if (!artifactsInstalled || state === 'unavailable') {
    return {
      statusKey: 'config.vdd_vulkan_hdr_bridge_status_unavailable',
      tone: 'muted',
      canValidate: false,
      showValidateAction: false,
      showValidateHint: false,
    }
  }

  const showValidateAction = state !== 'enabled'
  const canValidate = showValidateAction && displayAvailable && !validating.value
  if (state === 'idle' && !displayAvailable) {
    return {
      statusKey: 'config.vdd_vulkan_hdr_bridge_status_waiting_display',
      tone: 'muted',
      canValidate,
      showValidateAction,
      showValidateHint: true,
    }
  }

  const presentation = {
    idle: { statusKey: 'config.vdd_vulkan_hdr_bridge_status_idle', tone: 'ready' },
    validating: { statusKey: 'config.vdd_vulkan_hdr_bridge_status_validating', tone: 'info' },
    enabled: { statusKey: 'config.vdd_vulkan_hdr_bridge_status_enabled', tone: 'success' },
    error: { statusKey: 'config.vdd_vulkan_hdr_bridge_status_error', tone: 'warning' },
  }[state] ?? { statusKey: 'config.vdd_vulkan_hdr_bridge_status_idle', tone: 'ready' }

  return { ...presentation, canValidate, showValidateAction, showValidateHint: false }
})

async function refreshStatus() {
  try {
    const response = await fetch('/api/vulkan-hdr-bridge')
    if (!response.ok) throw new Error(`HTTP ${response.status}`)
    const result = await response.json()
    if (result.status?.toString() === 'true') status.value = result
  } catch (_) {
    status.value = createStatus({ state: 'unavailable' })
  } finally {
    statusLoaded.value = true
  }
}

async function validateBridge() {
  validating.value = true
  status.value = { ...status.value, state: 'validating' }
  try {
    const response = await fetch('/api/vulkan-hdr-bridge/validate', { method: 'POST' })
    if (!response.ok) throw new Error(`HTTP ${response.status}`)
    status.value = await response.json()
  } catch (_) {
    status.value = { ...status.value, state: 'error' }
  } finally {
    validating.value = false
    statusLoaded.value = true
  }
}

function scheduleRefresh() {
  if (!statusActive) return
  if (statusTimer !== undefined) window.clearTimeout(statusTimer)
  const delay = status.value.state === 'enabled' ? 3000 : 10000
  statusTimer = window.setTimeout(async () => {
    if (!document.hidden) await refreshStatus()
    scheduleRefresh()
  }, delay)
}

async function handleVisibilityChange() {
  if (!statusActive || document.hidden) return
  await refreshStatus()
  scheduleRefresh()
}

onMounted(async () => {
  statusActive = true
  document.addEventListener('visibilitychange', handleVisibilityChange)
  await refreshStatus()
  scheduleRefresh()
})

onUnmounted(() => {
  statusActive = false
  if (statusTimer !== undefined) window.clearTimeout(statusTimer)
  document.removeEventListener('visibilitychange', handleVisibilityChange)
})
</script>

<template>
  <MSwitchItem v-model="enabled" data-cfg="vdd_vulkan_hdr_bridge" :title="$tp('config.vdd_vulkan_hdr_bridge')" :info="$tp('config.vdd_vulkan_hdr_bridge_desc')" />
  <MItem :icon="({ success: 'check_circle', warning: 'warning', info: 'sync', ready: 'check_circle', muted: 'info' })[viewState.tone] || 'info'" :title="$tp(viewState.statusKey)" />
  <MItem
    v-if="viewState.showValidateAction"
    action
    icon="play_arrow"
    :disabled="!viewState.canValidate"
    :title="$tp(validating ? 'config.vdd_vulkan_hdr_bridge_validating' : 'config.vdd_vulkan_hdr_bridge_validate')"
    :text="viewState.showValidateHint ? $tp('config.vdd_vulkan_hdr_bridge_validate_hint') : ''"
    @click="validateBridge"
  />
</template>
