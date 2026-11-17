<script setup>
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { MGroup, MInfo, MItem, MSection, MSeg, MStatusPills } from 'foundation-ui/vue'
import { useI18n } from 'vue-i18n'

const props = defineProps({
  platform: String,
  config: Object,
})

const { t } = useI18n()
const config = ref(props.config)
const hdrRuntimeStatus = ref(null)
const hdrRuntimeStatusLoaded = ref(false)
let hdrRuntimeStatusTimer
let hdrRuntimeStatusActive = false

const hdrAnalysisMode = computed({
  get: () => {
    const value = config.value.hdr_luminance_analysis
    if (value === true || value === 'true' || value === 'enabled' || value === 'on') return 'on'
    if (value === false || value === 'false' || value === 'disabled' || value === 'off') return 'off'
    return 'auto'
  },
  set: (mode) => {
    config.value.hdr_luminance_analysis = mode
  },
})

const activeHdrPipeline = computed(() => {
  const pipelines = hdrRuntimeStatus.value?.pipelines ?? []
  return pipelines.find((pipeline) => pipeline.hdr_mode !== 'sdr') ?? pipelines[0] ?? null
})

const hdrCardDisabled = computed(
  () => hdrAnalysisMode.value === 'off' && !activeHdrPipeline.value?.analysis_active,
)

const hdrRuntimeViewState = computed(() => {
  if (!hdrRuntimeStatusLoaded.value) {
    return { statusKey: 'config.hdr_runtime_status_loading', tone: 'muted' }
  }
  if (!hdrRuntimeStatus.value?.available) {
    return hdrAnalysisMode.value === 'off'
      ? { statusKey: '_common.disabled', tone: 'muted' }
      : { statusKey: 'config.hdr_runtime_status_unavailable', tone: 'muted' }
  }

  const pipeline = activeHdrPipeline.value
  if (!pipeline) {
    return hdrAnalysisMode.value === 'off'
      ? { statusKey: '_common.disabled', tone: 'muted' }
      : { statusKey: 'config.hdr_runtime_status_waiting', tone: 'ready' }
  }
  if (pipeline.hdr_mode === 'sdr') {
    return { statusKey: 'config.hdr_runtime_status_sdr', tone: 'muted' }
  }
  if (pipeline.analysis_failure_reason) {
    // In auto mode these two are the pipeline working as designed, not a problem to
    // report: the encoder cannot carry dynamic metadata, or the transfer function
    // and codec in use have no format between them (HLG over AV1).
    const expectedFallback =
      pipeline.analysis_mode === 'auto' &&
      (pipeline.analysis_failure_reason === 'encoder_unsupported' ||
        pipeline.analysis_failure_reason === 'format_unsupported')
    return {
      statusKey: 'config.hdr_runtime_status_fallback',
      tone: expectedFallback ? 'muted' : 'warning',
    }
  }
  if (!pipeline.analysis_active) {
    return hdrAnalysisMode.value === 'off'
      ? { statusKey: '_common.disabled', tone: 'muted' }
      : { statusKey: 'config.hdr_runtime_status_fallback', tone: 'warning' }
  }
  if (!pipeline.scene_metadata_active) {
    return { statusKey: 'config.hdr_runtime_status_starting', tone: 'info' }
  }
  return { statusKey: 'config.hdr_runtime_status_active', tone: 'success' }
})

const hdrRuntimeBadges = computed(() => {
  const pipeline = activeHdrPipeline.value
  if (!pipeline || pipeline.hdr_mode === 'sdr') return []

  const badges = [pipeline.hdr_mode.toUpperCase()]
  if (pipeline.analysis_active) {
    for (const format of pipeline.metadata_formats ?? []) {
      badges.push(format === 'hdr10_plus' ? 'HDR10+' : format === 'hdr_vivid' ? 'HDR Vivid' : format)
    }
  }
  return badges
})

const hdrRuntimeConversionLabel = computed(() => {
  const path = activeHdrPipeline.value?.conversion_path
  if (!path) return ''
  if (path === 'amf_vpe_preferred') return t('config.amd_vpe_runtime')
  if (path === 'compute_shader_direct') return 'D3D11 Compute · Direct'
  if (path === 'compute_shader_scratch') return 'D3D11 Compute · Copy'
  return 'D3D11 Pixel Shader'
})

async function handleVisibilityChange() {
  if (!hdrRuntimeStatusActive || document.hidden) return
  await refreshHdrRuntimeStatus()
  scheduleHdrRuntimeStatusRefresh()
}

async function refreshHdrRuntimeStatus() {
  try {
    const response = await fetch('/api/runtime/hdr')
    if (!response.ok) throw new Error(`HTTP ${response.status}`)
    hdrRuntimeStatus.value = await response.json()
  } catch (_) {
    hdrRuntimeStatus.value = { available: false, pipelines: [] }
  } finally {
    hdrRuntimeStatusLoaded.value = true
  }
}

function scheduleHdrRuntimeStatusRefresh() {
  if (!hdrRuntimeStatusActive) return
  if (hdrRuntimeStatusTimer !== undefined) window.clearTimeout(hdrRuntimeStatusTimer)
  const delay = activeHdrPipeline.value ? 3000 : 10000
  hdrRuntimeStatusTimer = window.setTimeout(async () => {
    if (!document.hidden) await refreshHdrRuntimeStatus()
    scheduleHdrRuntimeStatusRefresh()
  }, delay)
}

onMounted(async () => {
  if (props.platform !== 'windows') return
  hdrRuntimeStatusActive = true
  document.addEventListener('visibilitychange', handleVisibilityChange)
  await refreshHdrRuntimeStatus()
  scheduleHdrRuntimeStatusRefresh()
})

onUnmounted(() => {
  hdrRuntimeStatusActive = false
  if (hdrRuntimeStatusTimer !== undefined) window.clearTimeout(hdrRuntimeStatusTimer)
  document.removeEventListener('visibilitychange', handleVisibilityChange)
})

const hdrModes = computed(() => [
  { value: 'auto', label: t('config.hdr_luminance_analysis_auto') },
  { value: 'on', label: t('_common.enabled') },
  { value: 'off', label: t('_common.disabled') },
])
const hdrStateIcon = computed(() => ({ success: 'check_circle', warning: 'warning', info: 'sync', ready: 'check_circle', muted: 'info' }[hdrRuntimeViewState.value.tone] || 'info'))
// the runtime state of the analysis as labelled pills (what it does now, the conversion in use, the badges of the pipeline)
const statePills = computed(() => [
  { id: 'state', icon: hdrStateIcon.value, label: t(hdrRuntimeViewState.value.statusKey), tone: ({ ready: 'ok', success: 'ok', warning: 'warn', muted: 'off' })[hdrRuntimeViewState.value.tone] || '' },
  ...(hdrRuntimeConversionLabel.value ? [{ id: 'conv', icon: 'swap_horiz', label: t('config.capture_compute_shader'), value: hdrRuntimeConversionLabel.value }] : []),
  ...hdrRuntimeBadges.value.map((badge) => ({ id: `b-${badge}`, icon: 'tag', label: badge })),
])
</script>

<template>
  <template v-if="platform === 'windows'">
    <MSection>
      <MGroup>
        <MItem data-cfg="hdr_luminance_analysis" icon="hdr_auto" :title="$t('config.hdr_luminance_analysis')" stack>
          <template #trail><MInfo :text="$t('config.hdr_luminance_analysis_desc')" /></template>
          <template #control><MSeg v-model="hdrAnalysisMode" block :label="$t('config.hdr_luminance_analysis')" :options="hdrModes" /></template>
        </MItem>
      </MGroup>
      <MStatusPills :items="statePills" :label="$t(hdrRuntimeViewState.statusKey)" />
    </MSection>
  </template>
</template>
