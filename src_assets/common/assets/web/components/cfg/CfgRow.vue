<script setup>
import { inject } from 'vue'
import { MItem } from 'foundation-ui/vue'

// The row of a tab page that opens a section as a page of its own (the page is the CfgSub with the same id).
const props = defineProps({
  id: { type: String, required: true },
  title: { type: String, required: true },
  text: { type: String, default: '' },
  icon: { type: String, default: 'tune' },
})
const nav = inject('cfgNav')
</script>

<template>
  <MItem action :icon="icon" :title="title" :text="text" :data-cfg-sub="id" @click="nav.open(props.id)" />
</template>
