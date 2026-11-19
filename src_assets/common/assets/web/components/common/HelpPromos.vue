<script setup>
import { inject, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { MPromo } from 'foundation-ui/vue'
import SponsorSheet from './SponsorSheet.vue'
import ResourceSheet from './ResourceSheet.vue'

// Download clients, sponsors and the resource links as promotional cards (end of the help pages).
const { t } = useI18n()
const shell = inject('webShell', null)
const sponsors = ref(false)
const links = ref(false)
const toDownloads = () => (shell?.nav ? shell.nav.go('downloads') : (window.location.href = '/downloads'))
</script>

<template>
  <div class="m-promos">
    <MPromo icon="download" :title="t('resource_card.client_downloads')" @click="toDownloads" />
    <MPromo icon="favorite" :title="t('home.sponsor_title')" @click="sponsors = true" />
    <MPromo icon="groups" :title="t('home.links_title')" :text="t('home.promo_links')" @click="links = true" />
    <SponsorSheet v-model="sponsors" />
    <ResourceSheet v-model="links" />
  </div>
</template>
