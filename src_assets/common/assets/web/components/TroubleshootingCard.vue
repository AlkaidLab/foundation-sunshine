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
.card {
  border: none;
  border-radius: 12px;
  transition: transform 0.2s ease, box-shadow 0.2s ease;
}

.card:hover {
  transform: translateY(-2px);
  box-shadow: 0 8px 25px rgba(0, 0, 0, 0.1) !important;
}

.card-header {
  border-radius: 12px 12px 0 0 !important;
  padding: 1rem 1.25rem;
}

.card-title {
  font-size: 1rem;
  font-weight: 600;
}

.card-body {
  padding: 1.25rem;
}

.card-body p {
  font-size: 0.9rem;
  line-height: 1.6;
}

.card-body p.pre-line {
  white-space: pre-line;
}

.alert {
  border-radius: 8px;
  font-size: 0.9rem;
  padding: 0.75rem 1rem;
}
</style>
