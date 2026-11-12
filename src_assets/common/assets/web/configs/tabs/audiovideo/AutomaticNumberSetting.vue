<script setup>
import { computed } from 'vue'
import { MValueItem } from 'foundation-ui/vue'
import { useI18n } from 'vue-i18n'
import { constraintText } from 'foundation-ui/settings/index.js'

// A number where 0 means automatic: a row that says 自动 while it is 0, and the dialog it opens takes the number (0 for automatic again).
const props = defineProps({
  modelValue: [Number, String],
  id: { type: String, required: true },
  labelKey: { type: String, required: true },
  descriptionKey: { type: String, required: true },
  unit: { type: String, required: true },
  min: { type: Number, default: 0 },
  max: { type: Number, default: undefined },
  coerceNumber: Boolean,
})
const emit = defineEmits(['update:modelValue'])
const { t, locale } = useI18n()
const model = computed({
  get: () => props.modelValue ?? '',
  set: (value) => emit('update:modelValue', props.coerceNumber && value !== '' ? Number(value) : value),
})
const check = (value) => {
  if (value === '') return ''
  const n = Number(value)
  if (!Number.isFinite(n)) return constraintText(String(locale.value), { code: 'number' })
  return n < props.min || (props.max !== undefined && n > props.max) ? constraintText(String(locale.value), { code: 'range', min: props.min, max: props.max }) : ''
}
</script>

<template>
  <MValueItem v-model="model" :data-cfg="id" type="number" :label="t(labelKey)" :help="t(descriptionKey)" :suffix="unit" :min="min" :max="max" :zero-text="t('_common.auto')" :empty-text="t('_common.auto')" :check="check" :ok-label="t('_common.ok')" :cancel-label="t('_common.cancel')" />
</template>
