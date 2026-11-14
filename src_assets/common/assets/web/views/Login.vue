<template>
  <div class="m-app m-root"><MLoginPage :client="client" :locale="lang" :next="next" :languages="languages" :links="links" @language="chooseLanguage" @done="go" /></div>
</template>

<script setup>
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { MLoginPage } from 'foundation-ui/vue'
import { BILIBILI, PRODUCT, PROJECT_LINKS, brandUrl } from 'foundation-ui/about'
import { createAuthClient, fetchRequest, safeNext } from 'foundation-ui/auth'

// The sign-in page of the Web UI (/login). The host redirects a browser without a session here, with the path to come back to in `next` (same-origin paths only).
const { locale } = useI18n()
const client = createAuthClient({ request: fetchRequest() })
const next = new URLSearchParams(window.location.search).get('next') || ''
const go = (target) => window.location.replace(safeNext(target, '/'))
// the language of the page itself (its words are Chinese and English; any other language reads English): the one of the host, or the one chosen in the footer, which is remembered
const KEY = 'foundation-ui.login.lang'
let saved = ''
try { saved = localStorage.getItem(KEY) || '' } catch { /* storage unavailable */ }
const lang = ref(saved === 'zh' || saved === 'en' ? saved : /^zh/i.test(String(locale.value)) ? 'zh' : 'en')
const languages = [{ value: 'zh', label: '简体中文' }, { value: 'en', label: 'English' }]
const chooseLanguage = (value) => { lang.value = value === 'zh' ? 'zh' : 'en'; try { localStorage.setItem(KEY, lang.value) } catch { /* not remembered */ } }
const docs = computed(() => { const d = PROJECT_LINKS.find((l) => l.id === 'docs'); return lang.value === 'zh' ? d.zh || d.url : d.url })
// the footer: 官网 and B 站 first (the site is .cn for Chinese), then the documentation and the repository
const links = computed(() => [{ id: 'site', url: brandUrl('site', lang.value) }, { id: 'bilibili', url: BILIBILI.url }, { id: 'docs', url: docs.value }, { id: 'github', url: PRODUCT.repo }])
</script>
