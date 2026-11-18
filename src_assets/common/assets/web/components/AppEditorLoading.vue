<script setup>
import { MPage, MPageLoading } from 'foundation-ui/vue'
import { useI18n } from 'vue-i18n'

// The editor's sheet before the editor's chunk is there: the same side sheet with the same title and the page skeleton in it, so the form takes its place without a sheet appearing after a wait.
const props = defineProps({ app: { type: Object, default: null } })
const emit = defineEmits(['close'])
const { t } = useI18n()
const isNew = !props.app || props.app.index === -1
</script>

<template>
  <MPage wide class="app-page" :title="isNew ? t('apps.add_new') : t('apps.edit')" back :back-label="t('apps.applications_title')" @back="emit('close')">
    <MPageLoading :groups="3" />
  </MPage>
</template>
