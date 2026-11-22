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
// A stale or revoked session cookie is still a session verdict, even in the control panel. The button sends the person
// through the normal sign-in form and back here; the new session replaces the cookie that said signed out.
const toLogin = () => window.location.assign(loginUrl('/password'))
</script>
