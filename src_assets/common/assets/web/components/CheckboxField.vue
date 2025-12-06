<template>
  <div class="form-group-enhanced">
    <div class="form-check">
      <input type="checkbox" class="form-check-input" :id="id" v-model="checked" />
      <label :for="id" class="form-check-label">{{ label }}</label>
    </div>
    <div v-if="hint" class="field-hint">{{ hint }}</div>
  </div>
</template>

<script>
export default {
  name: 'CheckboxField',
  props: {
    id: { type: String, required: true },
    modelValue: { type: [Boolean, String], default: false },
    label: { type: String, required: true },
    hint: { type: String, default: '' },
  },
  emits: ['update:modelValue'],
  computed: {
    checked: {
      get() {
        return this.modelValue === true || this.modelValue === 'true'
      },
      set(val) {
        this.$emit('update:modelValue', val ? 'true' : 'false')
      },
    },
  },
}
</script>
