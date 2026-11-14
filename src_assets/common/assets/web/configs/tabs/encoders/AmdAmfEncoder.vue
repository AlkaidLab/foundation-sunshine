<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { MButton, MChoiceItem, MFooter, MGroup, MSection, MSliderItem, MValueItem } from 'foundation-ui/vue'
import CfgExpert from '../../../components/cfg/CfgExpert.vue'
import CfgSlider from '../../../components/cfg/CfgSlider.vue'
import CfgChoice from '../../../components/cfg/CfgChoice.vue'
import CfgField from '../../../components/cfg/CfgField.vue'
import CfgSwitch from '../../../components/cfg/CfgSwitch.vue'
import { keyedOptions } from '../../../utils/cfgOptions.js'
import { constraintText } from 'foundation-ui/settings/index.js'

const props = defineProps(['platform', 'config'])
const { t, locale } = useI18n()
// a fixed GOP length: a whole number of frames, 1 to 1000 (the host takes -1 for the encoder's own choice, which the mode above stands for)
const gopCheck = (value) => (Number.isInteger(Number(value)) && Number(value) >= 1 && Number(value) <= 1000 ? '' : constraintText(String(locale.value), { code: 'range', min: 1, max: 1000 }))

const hevcGopMode = computed({
  get() {
    const value = props.config.amd_hevc_gop_size
    if (value === undefined || String(value) === '-1') return 'driver'
    return String(value) === '0' ? 'on-demand' : 'fixed'
  },
  set(mode) {
    props.config.amd_hevc_gop_size = mode === 'driver' ? -1 : mode === 'on-demand' ? 0 : 60
  },
})
const qvbrQuality = computed({
  get: () => Number(props.config.amd_qvbr_quality || 23),
  set: (value) => { props.config.amd_qvbr_quality = value },
})
const resetAdvanced = () => {
  const settings = props.config
  settings.amd_high_motion_qb = ''
  settings.amd_lowlatency_mode = ''
  settings.amd_multi_hw_instance = ''
  settings.amd_input_queue_size = ''
  settings.amd_av1_latency_mode = ''
  settings.amd_hevc_gop_size = -1
}

const driver = computed(() => ({ value: '', label: t('config.amd_driver_default') }))
const usage = computed(() => [driver.value, ...keyedOptions(t, [
  ['transcoding', 'config.amd_usage_transcoding'], ['webcam', 'config.amd_usage_webcam'], ['lowlatency_high_quality', 'config.amd_usage_lowlatency_high_quality'],
  ['lowlatency', 'config.amd_usage_lowlatency'], ['ultralowlatency', 'config.amd_usage_ultralowlatency'],
])])
const rateControl = computed(() => [driver.value, ...keyedOptions(t, [
  ['cbr', 'config.amd_rc_cbr'], ['cqp', 'config.amd_rc_cqp'], ['vbr_latency', 'config.amd_rc_vbr_latency'], ['vbr_peak', 'config.amd_rc_vbr_peak'],
  ['qvbr', 'config.amd_rc_qvbr'], ['hqvbr', 'config.amd_rc_hqvbr'], ['hqcbr', 'config.amd_rc_hqcbr'],
])])
// "driver default", enabled, disabled
const tri = computed(() => [driver.value, ...keyedOptions(t, [['enabled', '_common.enabled'], ['disabled', '_common.disabled']])])
const quality = computed(() => [driver.value, ...keyedOptions(t, [['speed', 'config.amd_quality_speed'], ['balanced', 'config.amd_quality_balanced'], ['quality', 'config.amd_quality_quality']])])
const coders = computed(() => keyedOptions(t, [['auto', 'config.coder_auto'], ['cabac', 'config.coder_cabac'], ['cavlc', 'config.coder_cavlc']]))
const slices = computed(() => [{ value: '0', label: t('config.amd_slices_per_frame_auto') }, ...['1', '2', '3', '4'].map((n) => ({ value: n, label: n }))])
const gopModes = computed(() => keyedOptions(t, [['driver', 'config.amd_driver_default'], ['on-demand', 'config.amd_hevc_gop_on_demand'], ['fixed', 'config.amd_hevc_gop_fixed']]))
const av1Latency = computed(() => [driver.value, ...keyedOptions(t, [['1', 'config.amd_av1_latency_mode_power_saving'], ['2', 'config.amd_av1_latency_mode_real_time'], ['3', 'config.amd_av1_latency_mode_lowest']])])
</script>

<template>
  <MSection :title="$t('tabs.advanced')">
    <MGroup>
      <CfgExpert control="seg" :config="config" k="amd_usage" :options="usage" />
      <CfgExpert control="seg" v-if="platform === 'windows'" :config="config" k="amd_slices_per_frame" :options="slices" def="0" />
      <CfgExpert control="switch" v-if="platform === 'windows'" :config="config" k="amd_avcodec_compat" def="disabled" />
      <CfgExpert control="switch" v-if="platform === 'windows'" :config="config" k="amd_vpe" def="disabled" />
    </MGroup>
  </MSection>

  <MSection :title="$t('config.amd_rc_group')">
    <MGroup>
      <CfgExpert control="seg" :config="config" k="amd_rc" :options="rateControl" />
      <CfgExpert control="seg" :config="config" k="amd_enforce_hrd" :options="tri" def="" />
      <MSliderItem v-if="config.amd_rc === 'qvbr'" v-model="qvbrQuality" data-cfg="amd_qvbr_quality" :title="$t('config.amd_qvbr_quality')" :info="$t('config.amd_qvbr_quality_desc')" :min="1" :max="51" :step="1" :decimals="0" size="m" icon-start="remove" icon-end="add" />
    </MGroup>
  </MSection>

  <MSection :title="$t('config.amd_quality_group')">
    <MGroup>
      <CfgExpert control="seg" :config="config" k="amd_quality" :options="quality" />
      <CfgExpert control="seg" :config="config" k="amd_preanalysis" :options="tri" def="" />
      <CfgExpert control="seg" :config="config" k="amd_vbaq" :options="tri" def="" />
      <CfgExpert control="seg" :config="config" k="amd_coder" :options="coders" />
    </MGroup>
  </MSection>

  <MSection v-if="platform === 'windows'" :title="$t('config.amd_advanced_group')">
    <template #action><MButton variant="text" size="sm" icon="restart_alt" @click="resetAdvanced">{{ $t('config.amd_advanced_reset') }}</MButton></template>
    <MGroup>
      <MChoiceItem v-model="hevcGopMode" data-cfg="amd_hevc_gop_size" :title="$t('config.amd_hevc_gop_size')" :options="gopModes" :help="$t('config.amd_hevc_gop_size_desc')" :cancel-label="$t('_common.cancel')" />
      <MValueItem v-if="hevcGopMode === 'fixed'" v-model="config.amd_hevc_gop_size" type="number" min="1" max="1000" step="1" :label="$t('config.amd_hevc_gop_fixed')" :check="gopCheck" :ok-label="$t('_common.ok')" :cancel-label="$t('_common.cancel')" />
      <CfgExpert control="seg" :config="config" k="amd_high_motion_qb" :options="tri" def="" />
      <CfgExpert control="seg" :config="config" k="amd_lowlatency_mode" :options="tri" def="" />
      <CfgExpert control="seg" :config="config" k="amd_multi_hw_instance" :options="tri" def="" />
      <CfgExpert control="slider" :config="config" k="amd_input_queue_size" def="" />
      <CfgExpert control="seg" :config="config" k="amd_av1_latency_mode" :options="av1Latency" />
    </MGroup>
    <MFooter>{{ $t('config.amd_advanced_group_desc') }}</MFooter>
  </MSection>
</template>
