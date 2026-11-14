<script setup>
import { computed } from 'vue'
import { MDialog } from 'foundation-ui/vue'

const props = defineProps({
  show: { type: Boolean, default: false },
  dialogId: { type: String, required: true },
  title: { type: String, required: true },
  titleIcon: { type: String, default: '' },
  tone: { type: String, default: 'accent' },
  dismissible: { type: Boolean, default: true },
  closeLabel: { type: String, default: 'Close' },
  maxWidth: { type: String, default: '500px' },
})
const emit = defineEmits(['close'])

const icon = computed(() => {
  if (props.titleIcon) return props.titleIcon
  return props.tone === 'danger' || props.tone === 'warning' ? 'warning' : 'info'
})
const dialogTone = computed(() => (props.tone === 'accent' ? 'info' : props.tone))
const update = (open) => {
  if (!open && props.dismissible) emit('close')
}
</script>

<template>
  <MDialog :model-value="show" :title="title" :icon="icon" :tone="dialogTone" :style="{ '--m-dialog-width': maxWidth }" @update:model-value="update" @cancel="dismissible && emit('close')">
    <slot />
    <template #actions><slot name="actions" /></template>
  </MDialog>
</template>
