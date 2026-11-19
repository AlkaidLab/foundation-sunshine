<template>
  <div class="m-app m-root latency-clock-page">
    <MPage :title="T.title"><MLatencyClock :locale="String(locale)" @open-external="openExternalUrl" /></MPage>
  </div>
</template>

<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { MPage, MLatencyClock, latencyClockTexts } from 'foundation-ui/vue'
import { openExternalUrl } from '../utils/helpers.js'

// 延迟测试时钟 (help): the shared clock; 全屏 is the browser's full screen on the display this page is on (the panel can pick the streamed display).
const { locale } = useI18n()
const T = computed(() => latencyClockTexts(String(locale.value)))
</script>
