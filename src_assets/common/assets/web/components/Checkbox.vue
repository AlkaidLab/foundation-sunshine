<template>
  <div :class="containerClass">
    <div class="form-check">
      <input
        class="form-check-input"
        type="checkbox"
        :id="id"
        v-model="model"
        :true-value="trueValue"
        :false-value="falseValue"
      />
      <label class="form-check-label" :for="id">
        {{ t(`${localePrefix}.${id}`) }}
      </label>
    </div>
    <div class="form-text" v-if="hasDescription">
      {{ t(descKey) }}
    </div>
  </div>
</template>

<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'

const props = defineProps({
  id: {
    type: String,
    required: true
  },
  localePrefix: {
    type: String,
    default: 'config'
  },
  modelValue: {
    type: [String, Boolean],
    default: undefined
  },
  default: {
    type: [String, Boolean],
    default: undefined
  },
  trueValue: {
    type: [String, Boolean],
    default: true
  },
  falseValue: {
    type: [String, Boolean],
    default: false
  },
  containerClass: {
    type: String,
    default: ''
  }
})

const emit = defineEmits(['update:modelValue'])

const { t, te } = useI18n()

const model = computed({
  get: () => props.modelValue ?? props.default ?? props.falseValue,
  set: (val) => emit('update:modelValue', val)
})

const descKey = computed(() => `${props.localePrefix}.${props.id}_desc`)
const hasDescription = computed(() => te(descKey.value))
</script>

<style scoped>
</style>
