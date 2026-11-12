<script setup>
import { MItem } from 'foundation-ui/vue'

// One action of the troubleshooting page: icon, name, one line on what it does, the button at the end; the result of the
// action (done, failed) shows under the row.
defineProps({
  icon: { type: String, required: true },
  title: { type: String, required: true },
  description: { type: String, default: '' },
  preLine: { type: Boolean, default: false },
})
</script>

<template>
  <MItem :icon="icon" :title="title" stack>
    <span v-if="description" :class="{ 'pre-line': preLine }">{{ description }}</span>
    <template #trail><slot /></template>
    <template v-if="$slots.alerts" #control><slot name="alerts" /></template>
  </MItem>
</template>

<style>
.pre-line { white-space: pre-line; }
</style>
