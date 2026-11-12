<script setup>
import { inject } from 'vue'

// Content of a tab page itself: shown while no section page is open.
const nav = inject('cfgNav')
</script>

<template>
  <slot v-if="!nav.sub.value" />
</template>
