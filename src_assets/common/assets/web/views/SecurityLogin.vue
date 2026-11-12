<template>
  <MSecurityLogin :client="client" :locale="String(locale)" :signed-out="toLogin" />
</template>

<script setup>
import { useI18n } from 'vue-i18n'
import { MSecurityLogin } from 'foundation-ui/vue'
import { createAuthClient, fetchRequest, loginUrl } from 'foundation-ui/auth'

// 安全与登录: this sign-in, the other devices that are signed in, the password. The same page as in the control panel.
const { locale } = useI18n()
const client = createAuthClient({ request: fetchRequest() })
// In the control panel's frame the page is a trusted local surface (the host says `local`): there is no sign-in to go to, so there is no handler and the page draws no sign-in button.
// A browser keeps its normal sign-in and sign-out.
const inPanel = window.isTauri === true && window.parent !== window
const toLogin = inPanel ? null : () => window.location.assign(loginUrl('/'))
</script>
