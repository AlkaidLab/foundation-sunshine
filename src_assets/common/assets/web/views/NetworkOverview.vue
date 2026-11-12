<template>
  <div class="m-app m-root network-overview-page">
    <MPage :title="T.title">
      <MNetworkOverview :report="state.view.report" :status="state.view.status" :locale="String(locale)" @recheck="state.refresh(true)" @fix="onFix" />
    </MPage>
  </div>
</template>

<script setup>
// 系统 › 网络概览: the host's GET /api/network/overview (foundation-ui createNetworkOverview, the same state as the control panel page). The host asks the STUN servers only
// when this page opens or 重新检测 is pressed and keeps the result 10 minutes; a host without the API answers 404 and the page says it is not available.
// A fix the page cannot do itself opens 串流设置 › 网络, where the settings of each fix are.
import { computed, inject, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import { MPage, MNetworkOverview, createNetworkOverview, networkOverviewTexts } from 'foundation-ui/vue'
import { apiFetch } from '../utils/apiFetch.js'

const { locale } = useI18n()
const webShell = inject('webShell', null)
const T = computed(() => networkOverviewTexts(String(locale.value)))
const state = createNetworkOverview({
  request: async (path) => {
    const response = await apiFetch(path)
    return { ok: response.ok, status: response.status, json: await response.json().catch(() => null) }
  },
})
const onFix = () => webShell?.openConfig({ tab: 'network' })
onMounted(() => { void state.refresh() })
</script>
