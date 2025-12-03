<template>
  <div class="card shadow-sm mb-4">
    <div :class="['card-header', 'border-bottom-0', headerClass]">
      <h5 class="card-title mb-0">
        <i :class="iconClass"></i>
        {{ title }}
      </h5>
    </div>
    <div class="card-body">
      <p class="text-muted mb-3" :class="{ 'pre-line': preLine }">
        {{ description }}
      </p>
      <slot name="alerts" />
      <slot />
    </div>
  </div>
</template>

<script setup>
import { computed } from 'vue'

const props = defineProps({
  icon: {
    type: String,
    required: true,
  },
  color: {
    type: String,
    required: true,
    validator: (value) => ['warning', 'info', 'danger', 'secondary', 'primary', 'success'].includes(value),
  },
  title: {
    type: String,
    required: true,
  },
  description: {
    type: String,
    required: true,
  },
  preLine: {
    type: Boolean,
    default: false,
  },
})

const headerClass = computed(() => `bg-${props.color} bg-opacity-10`)
const iconClass = computed(() => `fas ${props.icon} text-${props.color} me-2`)
</script>

<style scoped>
.alert {
  border-radius: 8px;
  font-size: 0.9rem;
  padding: 0.75rem 1rem;
}
</style>
