<template>
  <div class="m-app m-root about-page">
    <MAbout
      :version="installed"
      :build="build"
      :channel="update.channel.value"
      :update="{ state: update.state.value, version: update.nextVersion.value || '', checkedAt: update.checkedAt.value }"
      :sponsors="SPONSORS"
      :locale="String(locale)"
      :on-refresh="refreshAnnouncements"
      :consent="telemetry ? telemetry.consent.value : ''"
      :errors="telemetry ? telemetry.errors.value : ''"
      :preview-loader="loadTelemetryPreview"
      :update-channel="updateChannel"
      @channel-change="onChannel"
      @consent="(v) => telemetry?.answer(v)"
      @errors="(v) => telemetry?.setErrors(v)"
      @check="onCheck"
      @open-update="sheet = true"
      @open="open"
    >
      <template #help><MSection><MGroup><MItem action icon="health_and_safety" :title="doctorTexts(String(locale)).start" @click="shell?.nav.go('doctor', { autostart: true })" /></MGroup></MSection><MFeedbackAccount :locale="String(locale)" /></template>
    </MAbout>
    <VersionCard
      v-model="sheet"
      :version="update.version.value"
      :github-version="update.githubVersion.value"
      :pre-release-version="update.preReleaseVersion.value"
      :notify-pre-releases="update.notifyPreReleases.value"
      :loading="update.loading.value"
      :check-failed="update.state.value === 'error'"
      :installed-version-not-stable="update.installedVersionNotStable.value"
      :stable-build-available="update.stableBuildAvailable.value"
      :pre-release-build-available="update.preReleaseBuildAvailable.value"
      :build-version-is-dirty="update.buildVersionIsDirty.value"
      @retry="update.check()"
    />
  </div>
</template>

<script setup>
import { computed, inject, onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { MAbout, MGroup, MItem, MSection, aboutTexts, useNotify, doctorTexts, MFeedbackAccount } from 'foundation-ui/vue'
import VersionCard from '../components/common/VersionCard.vue'
import { SPONSORS } from '../config/sponsors.js'
import { hostData } from '../composables/hostData.js'
import { openExternalUrl } from '../utils/helpers.js'
import { useUpdateStatus } from '../composables/useUpdateStatus.js'
import { apiJson } from '../utils/apiFetch.js'

// 关于: the shared about page with this product's data: the host's version, the real update check (the same one as the home page)
// and its update sheet. This page cannot install: the sheet says where the update comes from.
const { locale } = useI18n()
const update = useUpdateStatus()
const refreshAnnouncements = inject('announcementsRefresh', null)
const telemetry = inject('telemetryConsent', null)
const shell = inject('webShell', null)
// 隐私 › 查看将发送的内容: the records the host would send next, with the fields every batch carries
const loadTelemetryPreview = () => apiJson('/api/telemetry/preview')
const sheet = ref(false)
const hostVersion = ref('')
const build = ref(null) // the build identity the host recorded (GET /api/version of a host that has it); an older host answers without it and the page guesses as before
const installed = computed(() => update.version.value?.version || hostVersion.value)
onMounted(async () => {
  try { hostVersion.value = (await hostData.refresh('config'))?.version || '' } catch { /* the page shows no version */ }
  try { const v = await apiJson('/api/version'); build.value = v && typeof v === 'object' && v.commit ? v : null } catch { build.value = null }
  update.ensure()
})
async function onCheck() {
  await update.check()
  if (update.state.value === 'available') sheet.value = true
}
// 更新通道: the host's notify_pre_releases, shown once the first check has read it
const updateChannel = computed(() => (update.state.value === 'idle' ? '' : update.notifyPreReleases.value ? 'prerelease' : 'stable'))
async function onChannel(value) {
  if (!(await update.setPreReleases(value === 'prerelease'))) useNotify().snack(aboutTexts(String(locale.value)).channelFailed)
}
const open = (url) => { openExternalUrl(url).catch((error) => console.error('Failed to open external URL:', error)) }
</script>
