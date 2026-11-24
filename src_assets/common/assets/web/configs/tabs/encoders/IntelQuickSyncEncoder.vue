<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { MGroup, MSection } from 'foundation-ui/vue'
import CfgExpert from '../../../components/cfg/CfgExpert.vue'
import CfgSteps from '../../../components/cfg/CfgSteps.vue'
import CfgChoice from '../../../components/cfg/CfgChoice.vue'
import CfgSwitch from '../../../components/cfg/CfgSwitch.vue'
import { keyedOptions, localeOptions } from '../../../utils/cfgOptions.js'

defineProps(['platform', 'config'])
const { t } = useI18n()
const presets = computed(() => localeOptions(t, 'config.qsv_preset', ['veryfast', 'faster', 'fast', 'medium', 'slow', 'slower', 'slowest']))
const coders = computed(() => keyedOptions(t, [['auto', 'config.coder_auto'], ['cabac', 'config.coder_cabac'], ['cavlc', 'config.coder_cavlc']]))
</script>

<template>
  <MSection>
    <MGroup>
      <CfgSteps :config="config" k="qsv_preset" :options="presets" def="medium" icon="speed" />
    </MGroup>
  </MSection>
  <MSection :title="$t('tabs.advanced')">
    <MGroup>
      <CfgExpert control="seg" :config="config" k="qsv_coder" :options="coders" />
      <CfgExpert control="switch" :config="config" k="qsv_slow_hevc" def="disabled" />
    </MGroup>
  </MSection>
</template>
