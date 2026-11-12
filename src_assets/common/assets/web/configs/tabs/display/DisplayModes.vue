<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { MFooter, MGroup, MItem, MModePicker, MSeg, MSection, displayTreeTexts } from 'foundation-ui/vue'
import { $tp } from '../../../platform-i18n'
import CfgField from '../../../components/cfg/CfgField.vue'
import CfgSwitch from '../../../components/cfg/CfgSwitch.vue'

// 分辨率与刷新率: how the client's request is treated (follow it, or hold the current value), then 支持的分辨率 and 支持的刷新率, two lists of their own (a checklist of common values, a custom value
// in one row, the list the clients are offered below it). The virtual display shares them (the host writes its mode list from these two settings). The lists apply when they are changed, like the
// other settings of the page (useApplyNow); the stored text is the host's own (`[1920x1080,...]`, `[60,120]`).
const props = defineProps({ config: { type: Object, required: true }, resolutions: { type: Array, required: true }, fps: { type: Array, required: true } })
const { locale, t } = useI18n()
const x = computed(() => displayTreeTexts(locale.value))
const rules = computed(() => ['automatic', 'no_operation', 'manual'].map((value) => ({ value, label: value === 'automatic' ? x.value.policy.follow : value === 'no_operation' ? x.value.policy.fixed : x.value.policy.manual })))
const resolutionRule = computed({ get: () => props.config.resolution_change || 'automatic', set: (value) => { props.config.resolution_change = value } })
const refreshRule = computed({ get: () => props.config.refresh_rate_change || 'automatic', set: (value) => { props.config.refresh_rate_change = value } })
const setResolutions = (value) => { props.resolutions.splice(0, props.resolutions.length, ...value) }
const setRates = (value) => { props.fps.splice(0, props.fps.length, ...value) }
</script>

<template>
  <MSection :title="x.sections.modes.policy">
    <MGroup>
      <MItem data-cfg="resolution_change" :title="$tp('config.resolution_change')" icon="aspect_ratio" stack>
        <template #control><MSeg v-model="resolutionRule" block :label="$tp('config.resolution_change')" :options="rules" /></template>
      </MItem>
      <CfgField v-if="resolutionRule === 'manual'" :config="config" k="manual_resolution" label="config.resolution_change_manual" help="none" placeholder="2560x1440" />
      <MItem data-cfg="refresh_rate_change" :title="$tp('config.refresh_rate_change')" icon="speed" stack>
        <template #control><MSeg v-model="refreshRule" block :label="$tp('config.refresh_rate_change')" :options="rules" /></template>
      </MItem>
      <CfgField v-if="refreshRule === 'manual'" :config="config" k="manual_refresh_rate" label="config.refresh_rate_change_manual" help="none" placeholder="59.95" />
      <CfgSwitch :config="config" k="dynamic_resolution_follow_display" def="enabled" />
    </MGroup>
  </MSection>

  <MSection :title="x.sections.modes.resolutions">
    <MModePicker data-cfg="resolutions" kind="resolution" :title="$t('config.resolutions')" :model-value="resolutions" :locale="locale" @update:model-value="setResolutions" />
  </MSection>

  <MSection :title="x.sections.modes.rates">
    <MModePicker data-cfg="fps" kind="rate" :title="$t('config.fps')" :model-value="fps" :locale="locale" @update:model-value="setRates" />
    <MFooter>{{ t('config.res_fps_desc') }}</MFooter>
    <MFooter>{{ x.list.shared }}</MFooter>
  </MSection>
</template>
