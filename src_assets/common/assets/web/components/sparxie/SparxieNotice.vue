<template>
  <MBanner tone="warn" icon="warning" class="spx-notice"><b>{{ C.noticeTitle }}</b> {{ text }}</MBanner>
</template>

<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { MBanner } from 'foundation-ui/vue'
import { useSparxie } from '../../composables/useSparxie.js'
import { useSparxieCopy } from '../../composables/useSparxieCopy.js'
import { sparxieLang } from '../../utils/sparxie/copy.js'

// The statement about what Sparxie does to the game, in the words of the facts: shown on the page that creates the first application until the person has created one (the host then records that it was shown);
// the touch-launch group of every Sparxie application keeps the same statement as its footer. It is a statement, not a dialog: nothing is hidden behind a button.
const C = useSparxieCopy()
const { locale } = useI18n()
const { state } = useSparxie()
// the host sends the statement in both languages (it owns the wording); the copy of this page is the fallback
const text = computed(() => state.statement?.[sparxieLang(locale.value)] || C.value.notice)
</script>

<style scoped>
.spx-notice b { font-weight: 600; margin-right: 6px; }
</style>
