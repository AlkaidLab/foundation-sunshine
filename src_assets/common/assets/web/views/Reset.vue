<template>
  <div class="m-app m-root reset-page">
    <MResetSettings :locale="String(locale)" :host="host" :panel="null" :streaming="streaming" />
  </div>
</template>

<script setup>
import { ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { MResetSettings, createResetClient } from 'foundation-ui/vue'
import { apiFetch, apiJson } from '../utils/apiFetch.js'

// 恢复默认设置 (system): the host's settings back to the defaults, and the last backup back. The page and its words are foundation-ui's (the control panel shows the same one,
// with its own preferences added); the calls go through apiFetch, which sends the session cookie and the CSRF token.
const { locale } = useI18n()
const host = createResetClient((path, init) => apiFetch(path, init))
// a stream that is running ends when the host restarts: the question says so
const streaming = ref(false)
apiJson('/api/runtime/sessions').then((info) => { streaming.value = (info?.total_sessions || 0) > 0 }).catch(() => {})
</script>
