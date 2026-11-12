<script setup>
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { MButton, MExpertItem, MInfo, MItem, MPickSheet, MValueDialog } from 'foundation-ui/vue'
import { SETTING_META, isChanged } from 'foundation-ui/settings/index.js'
import { useCfgText } from '../../composables/useCfgText.js'
import { useCfgPin } from '../../composables/useCfgPin.js'
import { MANUAL_DEVICE, pickerOptions, pickerText, pickerValue } from '../../utils/audioDevices.js'

// One configuration setting that names an audio device: the row shows the device, tapping it opens a list of the
// devices the host reports (automatic first, the system default marked, virtual devices such as VB-CABLE labelled) and
// an entry for typing a name or an id by hand. A stored value the list does not know stays selectable and says so.
// `expert` makes the row an expert row (what it does, the default, a reset); the title is `config.<k>`, the help `config.<k>_desc`.
const props = defineProps({
  config: { type: Object, required: true },
  k: { type: String, required: true },
  /** the endpoints to choose from (see utils/audioDevices.js) */
  devices: { type: Array, default: () => [] },
  expert: { type: Boolean, default: false },
  label: { type: String, default: '' },
  help: { type: String, default: '' },
  tp: { type: Boolean, default: false },
  disabled: { type: Boolean, default: false },
  placeholder: { type: String, default: '' },
})
const { t, title, info } = useCfgText(props)
const { te } = useI18n()
const pin = useCfgPin(props, title)

const labels = computed(() => ({
  auto: t('_common.auto'),
  manual: t('config.audio_device_manual'),
  missing: t('config.audio_device_missing'),
  defaultTag: t('config.audio_device_default_tag'),
  kinds: {
    vb_cable: t('config.audio_device_kind_vb_cable'),
    vb_audio: t('config.audio_device_kind_vb_audio'),
    steam_streaming: t('config.audio_device_kind_steam'),
    virtual_microphone: t('config.audio_device_kind_mic'),
  },
  defaultNow: (name) => t('config.audio_device_default_now', { name }),
}))
const stored = computed(() => props.config[props.k])
const options = computed(() => pickerOptions(props.devices, stored.value, labels.value))
const valueText = computed(() => pickerText(props.devices, stored.value, t('_common.auto')))

const sheetOpen = ref(false)
const manualOpen = ref(false)
const choice = computed({
  get: () => pickerValue(props.devices, stored.value),
  set: (value) => {
    if (value === MANUAL_DEVICE) { manualOpen.value = true; return }
    props.config[props.k] = value
  },
})
const manual = computed({
  get: () => (stored.value === undefined || stored.value === null ? '' : String(stored.value)),
  set: (value) => { props.config[props.k] = value },
})

// the expert row: what changed from the default, and how to put it back
const meta = computed(() => SETTING_META[props.k] || {})
const changed = computed(() => isChanged(props.k, stored.value, meta.value.def))
const reset = () => { props.config[props.k] = meta.value.def ?? '' }
const effect = computed(() => (te(`config.effect.${props.k}`) ? t(`config.effect.${props.k}`) : ''))
const texts = computed(() => ({
  changed: t('config.expert_changed'), risk: t('config.expert_risk'), desc: t('config.expert_desc'), effect: t('config.expert_effect'),
  default: t('config.expert_default'), reset: t('config.expert_reset'),
}))
const defText = computed(() => t('_common.auto'))
</script>

<template>
  <MExpertItem v-if="expert" :data-cfg="k" :title="title" :value="valueText" :changed="changed" :risk="meta.risk || ''" :desc="info" :def-text="defText" :effect="effect" :disabled="disabled" :texts="texts" :pin="pin" @reset="reset">
    <MButton variant="tonal" size="sm" :disabled="disabled" @click="sheetOpen = true">{{ valueText }}</MButton>
  </MExpertItem>
  <MItem v-else :data-cfg="k" :pin="pin" action :title="title" :value="valueText" :disabled="disabled" @click="sheetOpen = true">
    <template v-if="info" #trail><MInfo :text="info" /></template>
  </MItem>
  <MPickSheet v-model="choice" v-model:open="sheetOpen" :title="title" :options="options" :search-label="$t('_common.search')" :empty-text="$t('_common.no_results')" :close-label="$t('_common.close')" />
  <MValueDialog v-model="manual" v-model:open="manualOpen" :title="title" :placeholder="placeholder" :help="$t('config.audio_device_manual_help')" :ok-label="$t('_common.ok')" :cancel-label="$t('_common.cancel')" />
</template>
