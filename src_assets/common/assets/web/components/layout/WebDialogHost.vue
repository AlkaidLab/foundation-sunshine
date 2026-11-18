<script setup>
import { useI18n } from 'vue-i18n'
import { MDialogHost } from 'foundation-ui/vue'

// The place of the questions of the page (useDialogs): the embedded page and the Web UI shell both mount it, so no question is a browser dialog.
const { locale } = useI18n()
</script>

<template>
  <MDialogHost :locale="String(locale)" />
</template>
