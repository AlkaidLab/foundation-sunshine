<script setup>
import { inject } from 'vue'
import { useI18n } from 'vue-i18n'
import { MNotifyHost, MPendingHost, usePending } from 'foundation-ui/vue'

// The one bar of unsaved changes, its list and its save confirmation, and the place of the snackbars: mounted once by the shell.
const { locale } = useI18n()
const shell = inject('webShell', null)
const pending = usePending()
// a page of the changes: the settings tab (with the place), the gamepad, the application list
function go(page) {
  const entry = pending.all.value.find((e) => e.page === page)
  if (page.startsWith('config:')) shell?.openConfig({ tab: entry?.tab || page.slice(7), sub: entry?.sub, focus: entry?.focus })
  else shell?.nav.go(page)
}
</script>

<template>
  <MNotifyHost />
  <MPendingHost :locale="String(locale)" @go="go" />
</template>
