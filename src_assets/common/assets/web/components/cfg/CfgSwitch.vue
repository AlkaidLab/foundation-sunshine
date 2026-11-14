<script setup>
import { computed } from 'vue'
import { useCfgText } from '../../composables/useCfgText.js'
import { MSwitchItem } from 'foundation-ui/vue'
import { useCfgPin } from '../../composables/useCfgPin.js'

// One configuration setting as a switch row. The value kept in the configuration is `on` or `off`
// ('enabled' / 'disabled' by default); `def` is what an unset key means.
const props = defineProps({
  config: { type: Object, required: true },
  k: { type: String, required: true },
  on: { type: [String, Boolean], default: 'enabled' },
  off: { type: [String, Boolean], default: 'disabled' },
  def: { type: [String, Boolean], default: undefined },
  /** full locale key of the title (default config.<k>) */
  label: { type: String, default: '' },
  /** full locale key of the help text (default config.<k>_desc, shown when it exists); 'none' hides it */
  help: { type: String, default: '' },
  icon: { type: String, default: '' },
  /** use the platform variant of the locale keys */
  tp: { type: Boolean, default: false },
  disabled: { type: Boolean, default: false },
})
const { title, info } = useCfgText(props)
const pin = useCfgPin(props, title)
const settings = () => props.config
const model = computed({
  get: () => String(settings()[props.k] ?? props.def ?? props.off) === String(props.on),
  set: (value) => { settings()[props.k] = value ? props.on : props.off },
})
</script>

<template>
  <MSwitchItem v-model="model" :data-cfg="k" :title="title" :icon="icon" :info="info" :disabled="disabled" :pin="pin">
    <template #extra><slot /></template>
  </MSwitchItem>
</template>
