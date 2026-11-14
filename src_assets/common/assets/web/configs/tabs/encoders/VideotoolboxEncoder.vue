<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { MGroup, MSection } from 'foundation-ui/vue'
import CfgExpert from '../../../components/cfg/CfgExpert.vue'
import CfgSlider from '../../../components/cfg/CfgSlider.vue'
import CfgChoice from '../../../components/cfg/CfgChoice.vue'
import CfgSwitch from '../../../components/cfg/CfgSwitch.vue'
import { keyedOptions } from '../../../utils/cfgOptions.js'

defineProps(['platform', 'config'])
const { t } = useI18n()
const coders = computed(() => keyedOptions(t, [['auto', 'config.coder_auto'], ['cabac', 'config.coder_cabac'], ['cavlc', 'config.coder_cavlc']]))
const software = computed(() => keyedOptions(t, [['auto', '_common.auto'], ['disabled', '_common.disabled'], ['allowed', 'config.vt_software_allowed'], ['forced', 'config.vt_software_forced']]))
</script>

<template>
  <MSection :title="$t('tabs.advanced')">
    <MGroup>
      <CfgExpert control="seg" :config="config" k="vt_coder" :options="coders" />
      <CfgExpert control="seg" :config="config" k="vt_software" :options="software" />
      <CfgExpert control="switch" :config="config" k="vt_realtime" def="enabled" />
    </MGroup>
  </MSection>
</template>
