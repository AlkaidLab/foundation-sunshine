<template>
  <MPage :title="title"><MAppearance :locale="String(locale)" /></MPage>
</template>

<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { MAppearance, MPage } from 'foundation-ui/vue'
import { labelOf } from 'foundation-ui/settings/index.js'

// 个性化: a destination of its own (the palette button in the foot of the rail), the same shared component as in the control panel
const { locale } = useI18n()
const title = computed(() => labelOf(String(locale.value), 'shell.appearance'))
</script>
