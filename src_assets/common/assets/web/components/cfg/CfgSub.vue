<script setup>
import { inject, watchEffect } from 'vue'

// One section of a configuration tab as a page of its own: shown while the section with this id is open.
// `inline`: not a page of its own. Its content is part of the tab page, below the rest of it, so every setting of the tab is on the page that the list row opens.
const props = defineProps({
  id: { type: String, required: true },
  title: { type: String, required: true },
  inline: { type: Boolean, default: false },
})
const nav = inject('cfgNav')
watchEffect(() => { if (!props.inline && nav.sub.value === props.id) nav.title.value = props.title })
</script>

<template>
  <div v-if="inline ? !nav.sub.value : nav.sub.value === id" class="cfg-sub"><slot /></div>
</template>
