<script setup>
import { computed } from 'vue'
import { useCfgText } from '../../composables/useCfgText.js'
import { MValueItem } from 'foundation-ui/vue'
import { constraintText, validateSetting } from 'foundation-ui/settings/index.js'
import { useI18n } from 'vue-i18n'

// One configuration setting with a typed value (text or number): a row with the value; the field is in the dialog the row opens.
const props = defineProps({
  config: { type: Object, required: true },
  k: { type: String, required: true },
  type: { type: String, default: 'text' },
  label: { type: String, default: '' },
  help: { type: String, default: '' },
  placeholder: { type: [String, Number], default: '' },
  suffix: { type: String, default: '' },
  min: { type: [Number, String], default: undefined },
  max: { type: [Number, String], default: undefined },
  step: { type: [Number, String], default: undefined },
  /** use the platform variant of the locale keys */
  tp: { type: Boolean, default: false },
  disabled: { type: Boolean, default: false },
  error: { type: String, default: '' },
})
const { title, info } = useCfgText(props)
const { locale, t } = useI18n()
const settings = () => props.config
const model = computed({
  get: () => settings()[props.k] ?? '',
  set: (value) => { settings()[props.k] = value },
})
// what the host takes for this key (src/config.cpp): the dialog says so at once and keeps 确定 off, the row shows a stored value that breaks it, and the change is marked invalid in the unsaved list
const check = (value) => constraintText(String(locale.value), validateSetting(props.k, value))
</script>

<template>
  <MValueItem
    v-model="model"
    :data-cfg="k"
    :type="type"
    :label="title"
    :placeholder="String(placeholder)"
    :suffix="suffix"
    :min="min"
    :max="max"
    :step="step"
    :disabled="disabled"
    :error="error"
    :check="check"
    :help="info"
    :ok-label="t('_common.ok')"
    :cancel-label="t('_common.cancel')"
    :empty-text="t('_common.auto')"
  />
</template>
