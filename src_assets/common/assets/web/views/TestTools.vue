<template>
  <div class="m-app m-root test-tools-page">
    <MPage :title="T.title"><MTestTools :locale="String(locale)" :host-rumble="hostRumble" /></MPage>
  </div>
</template>

<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { MPage, MTestTools, testToolTexts } from 'foundation-ui/vue'
import { apiPostJson } from '../utils/apiFetch.js'

// 测试工具 (help): the shared page of the six small tools. They judge what the browser of the client does (its display, its speakers, its input), which is what a stream is seen and heard on.
const { locale } = useI18n()
const T = computed(() => testToolTexts(String(locale.value)))
// The gamepad tool's rumble: a pad that exists only during a stream is not reachable from the browser, so the host sends one short pulse through the stream (the answer: sent / no_pad / busy / unsupported).
const hostRumble = (body = {}) => apiPostJson('/api/dualsense/rumble-test', body)
</script>
