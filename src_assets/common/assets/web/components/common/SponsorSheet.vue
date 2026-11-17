<script setup>
import { useI18n } from 'vue-i18n'
import { MGroup, MIcon, MItem, MSheet } from 'foundation-ui/vue'
import { SPONSORS } from '../../config/sponsors.js'
import { openExternalUrl } from '../../utils/helpers.js'

// One row per developer; the targets live in config/sponsors.js.
const open = defineModel({ type: Boolean, default: false })
const { t } = useI18n()
const go = (sponsor) => openExternalUrl(sponsor.url).catch((error) => console.error('Failed to open sponsor link:', error))
</script>

<template>
  <MSheet v-model="open" :title="t('home.sponsor_title')" side="end" :close-label="t('_common.close')">
    <MGroup>
      <MItem v-for="sponsor in SPONSORS" :key="sponsor.id" action :title="sponsor.label" @click="go(sponsor)">
        <template #lead><span class="m-lead-circle m-lead-circle--tertiary" aria-hidden="true"><MIcon name="favorite" fill /></span></template>
        <template #trail><MIcon name="open_in_new" /></template>
      </MItem>
    </MGroup>
  </MSheet>
</template>
