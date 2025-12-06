<template>
  <div class="form-group-enhanced">
    <label :for="id" class="form-label-enhanced" :class="{ 'required-field': required }">{{ label }}</label>
    <slot></slot>
    <div v-if="validation && !validation.isValid" class="invalid-feedback">{{ validation.message }}</div>
    <div v-if="validation && validation.isValid && value" class="valid-feedback">有效</div>
    <div v-if="$slots.hint" class="field-hint"><slot name="hint"></slot></div>
    <div v-else-if="hint" class="field-hint">{{ hint }}</div>
  </div>
</template>

<script>
export default {
  name: 'FormField',
  props: {
    id: { type: String, required: true },
    label: { type: String, required: true },
    hint: { type: String, default: '' },
    validation: { type: Object, default: null },
    value: { type: [String, Number], default: '' },
    required: { type: Boolean, default: false },
  },
}
</script>

<style scoped>
.required-field::after {
  content: ' *';
  color: #dc3545;
}

.invalid-feedback,
.valid-feedback {
  display: block;
  font-size: 0.875rem;
  margin-top: 0.25rem;
}

.invalid-feedback {
  color: #dc3545;
}

.valid-feedback {
  color: #198754;
}
</style>
