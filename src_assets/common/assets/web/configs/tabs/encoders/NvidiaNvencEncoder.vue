<script setup>
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { MBanner, MFooter, MGroup, MSection } from 'foundation-ui/vue'
import CfgExpert from '../../../components/cfg/CfgExpert.vue'
import CfgSteps from '../../../components/cfg/CfgSteps.vue'
import CfgChoice from '../../../components/cfg/CfgChoice.vue'
import CfgField from '../../../components/cfg/CfgField.vue'
import CfgSwitch from '../../../components/cfg/CfgSwitch.vue'
import { keyedOptions } from '../../../utils/cfgOptions.js'

const props = defineProps([
  'platform',
  'config',
])

const { t } = useI18n()
const config = ref(props.config)

// Runtime report of the last real session's frame budget guard evaluation
// (read-only field injected by the backend into /api/config).
const frameBudget = computed(() => {
  const report = config.value?.active_nvenc_frame_budget
  if (!report || report.effective_preset === undefined) return null
  return {
    ...report,
    fpsText: Math.round(report.fps),
    budgetText: Number(report.budget_ms).toFixed(1),
    estimatedText: Number(report.configured_estimated_ms).toFixed(1),
  }
})

const presets = computed(() => [1, 2, 3, 4, 5, 6, 7].map((n) => ({
  value: String(n),
  label: [1, 4, 7].includes(n) ? `P${n} ${t(`config.nvenc_preset_${n}`)}` : `P${n}`,
})))
const presetSegments = computed(() => presets.value.map((o) => ({ value: o.value, label: `P${o.value}` })))
const twopass = computed(() => keyedOptions(t, [['disabled', 'config.nvenc_twopass_disabled'], ['quarter_res', 'config.nvenc_twopass_quarter_res'], ['full_res', 'config.nvenc_twopass_full_res']]))
const rateControl = computed(() => keyedOptions(t, [['cbr', 'config.nvenc_rate_control_cbr'], ['vbr', 'config.nvenc_rate_control_vbr']]))
const splitEncode = computed(() => keyedOptions(t, [
  ['disabled', '_common.disabled'],
  ['driver_decides', 'config.nvenc_split_encode_driver_decides_def'],
  ['enabled', '_common.enabled'],
  ['two_strips', 'config.nvenc_split_encode_two_strips'],
  ['three_strips', 'config.nvenc_split_encode_three_strips'],
  ['four_strips', 'config.nvenc_split_encode_four_strips'],
]))
const driverOff = computed(() => config.value.nvenc_manage_driver_settings === 'disabled')
</script>
<template>
  <MBanner v-if="frameBudget && frameBudget.clamped" tone="warn" icon="warning">
    {{
      $t('config.nvenc_frame_budget_clamped', {
        configured: frameBudget.configured_preset,
        effective: frameBudget.effective_preset,
        width: frameBudget.width,
        height: frameBudget.height,
        fps: frameBudget.fpsText,
        budget: frameBudget.budgetText,
        estimated: frameBudget.estimatedText,
      })
    }}
  </MBanner>

  <MSection>
    <MGroup>
      <CfgSteps :config="config" k="nvenc_preset" :options="presetSegments" def="4" :axis-start="$t('config.preset_faster')" :axis-end="$t('config.preset_better')" icon="speed" />
    </MGroup>
  </MSection>
  <MSection :title="$t('tabs.advanced')">
    <MGroup>
      <CfgExpert control="switch" :config="config" k="nvenc_frame_budget_guard" def="enabled" />
      <CfgExpert control="seg" :config="config" k="nvenc_twopass" :options="twopass" />
      <CfgExpert control="switch" :config="config" k="nvenc_spatial_aq" def="enabled" />
      <CfgExpert control="seg" :config="config" k="nvenc_rate_control" :options="rateControl" />
      <CfgExpert control="slider" v-if="config.nvenc_rate_control === 'vbr'" :config="config" k="nvenc_target_quality" />
      <CfgExpert control="slider" :config="config" k="nvenc_vbv_increase" />
    </MGroup>
    <MFooter><a href="https://en.wikipedia.org/wiki/Video_buffering_verifier" target="_blank" rel="noopener noreferrer">VBV/HRD</a></MFooter>
  </MSection>

  <MSection :title="$t('config.misc')">
    <MGroup>
      <template v-if="platform === 'windows'">
        <CfgExpert control="switch" :config="config" k="nvenc_realtime_hags" def="enabled" />
        <CfgExpert control="seg" :config="config" k="nvenc_split_encode" :options="splitEncode" def="driver_decides" />
        <CfgExpert control="switch" :config="config" k="nvenc_manage_driver_settings" def="enabled" />
        <CfgExpert control="switch" :config="config" k="nvenc_latency_over_power" def="enabled" :disabled="driverOff" />
        <CfgExpert control="switch" :config="config" k="nvenc_opengl_vulkan_on_dxgi" def="enabled" :disabled="driverOff" />
        <CfgExpert control="switch" :config="config" k="nvenc_cuda_array_input" def="disabled" />
      </template>
      <CfgExpert control="switch" :config="config" k="nvenc_h264_cavlc" def="disabled" />
    </MGroup>
    <MFooter v-if="platform === 'windows'"><a href="https://devblogs.microsoft.com/directx/hardware-accelerated-gpu-scheduling/" target="_blank" rel="noopener noreferrer">HAGS</a></MFooter>
  </MSection>
</template>
