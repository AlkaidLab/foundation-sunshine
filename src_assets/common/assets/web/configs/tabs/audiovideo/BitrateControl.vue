<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { MChipSet, MInfo, MItem, MSlider } from 'foundation-ui/vue'
import { BITRATE_SLIDER_MAX_PROGRESS, bitrateChips, bitrateFromProgress, bitrateKbps, bitrateSliderProgress, bitrateText, bitrateChipMarks, BITRATE_PRESETS_MBPS } from 'foundation-ui/settings'

// The bitrate ceiling (max_bitrate, Kbps; 0 = automatic, the client decides): presets as chips for the usual choices, the slider for
// anything between. Tier 1 of 音频/视频. The rules (what 0 and an unset key mean, whole Kbps, the custom chip) are the shared ones of
// foundation-ui settings/bitrate.js, the same as the panel's 串流 page.
const props = defineProps({ config: { type: Object, required: true } })
const { t } = useI18n()
const kbps = computed(() => bitrateKbps(props.config.max_bitrate))
// showing a stored value never rewrites it: only a chip or the slider does
const progress = computed({
  get: () => bitrateSliderProgress(kbps.value),
  set: (value) => { props.config.max_bitrate = bitrateFromProgress(value) },
})
// the marks of the slider stand under the values of the chips
const chipMarks = bitrateChipMarks(BITRATE_PRESETS_MBPS)
const chips = computed(() => bitrateChips(kbps.value, t('_common.auto')))
const chip = computed({
  get: () => kbps.value,
  set: (value) => { props.config.max_bitrate = bitrateKbps(value) },
})
</script>

<template>
  <MItem data-cfg="max_bitrate" :title="$t('config.max_bitrate')" icon="speed" stack>
    <template #trail><MInfo :text="$t('config.max_bitrate_desc')" /><span class="m-slider-value">{{ bitrateText(kbps, $t('_common.auto')) }}</span></template>
    <template #control>
      <div class="cfg-bitrate">
        <MChipSet v-model="chip" :options="chips" :label="$t('config.max_bitrate')" />
        <MSlider v-model="progress" :min="0" :max="BITRATE_SLIDER_MAX_PROGRESS" :step="1" unit="" size="m" icon-start="signal_cellular_alt_2_bar" icon-end="signal_cellular_alt" :marks="chipMarks" :off-text="$t('_common.auto')" :label="$t('config.max_bitrate')" />
      </div>
    </template>
  </MItem>
</template>

<style>
.cfg-bitrate { display: flex; flex-direction: column; gap: 12px; }
</style>
