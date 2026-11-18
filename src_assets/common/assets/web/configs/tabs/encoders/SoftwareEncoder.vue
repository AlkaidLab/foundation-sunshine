<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { MGroup, MSection } from 'foundation-ui/vue'
import CfgExpert from '../../../components/cfg/CfgExpert.vue'
import CfgSteps from '../../../components/cfg/CfgSteps.vue'
import CfgChoice from '../../../components/cfg/CfgChoice.vue'
import CfgField from '../../../components/cfg/CfgField.vue'
import { localeOptions } from '../../../utils/cfgOptions.js'

defineProps(['platform', 'config'])
const { t } = useI18n()
const presets = computed(() => localeOptions(t, 'config.sw_preset', ['ultrafast', 'superfast', 'veryfast', 'faster', 'fast', 'medium', 'slow', 'slower', 'veryslow']))
const tunes = computed(() => localeOptions(t, 'config.sw_tune', ['film', 'animation', 'grain', 'stillimage', 'fastdecode', 'zerolatency']))
</script>

<template>
  <MSection>
    <MGroup>
      <CfgSteps :config="config" k="sw_preset" :options="presets" def="superfast" icon="speed" />
    </MGroup>
  </MSection>
  <MSection :title="$t('tabs.advanced')">
    <MGroup>
      <CfgExpert control="seg" :config="config" k="sw_tune" :options="tunes" />
      <CfgExpert control="slider" :config="config" k="min_threads" />
    </MGroup>
  </MSection>
</template>
