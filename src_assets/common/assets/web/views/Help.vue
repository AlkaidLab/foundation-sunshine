<template>
  <div class="m-app m-root help-page">
    <MPage :title="t('shell.help_about')" :locale="String(locale)">
      <!-- 软件更新, 主机体检, 反馈问题 and 客户端下载 (the same tiles as the control panel's 帮助与关于); the update action is on its tile -->
      <MSection>
        <MHelpTiles
          :locale="String(locale)"
          :update="{ state: update.state.value, installed, next: update.nextVersion.value || '', checkedAt: update.checkedAt.value }"
          @check="update.check()"
          @open-update="shell?.nav.go('about', { openUpdate: true })"
          @doctor="shell?.nav.go('doctor', { autostart: true })"
          @downloads="shell?.nav.go('downloads')"
        />
      </MSection>
      <MSection>
        <MGroup>
          <MItem action icon="info" :title="aboutTexts(String(locale)).title" :text="aboutSub" @click="shell?.nav.go('about')" />
          <MItem action icon="bug_report" :title="t('shell.logs_diagnostics')" @click="shell?.nav.go('troubleshooting')" />
        </MGroup>
      </MSection>
    </MPage>
  </div>
</template>

<script setup>
import { computed, inject, onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { MPage, MSection, MGroup, MItem, MHelpTiles, aboutTexts } from 'foundation-ui/vue'
import { helpTileTexts, aboutLine } from 'foundation-ui/help-tiles'
import { hostData } from '../composables/hostData.js'
import { useUpdateStatus } from '../composables/useUpdateStatus.js'

// 帮助与关于: the four tiles first, then the way to 关于 (version, update section, privacy, licences). The update state is the one quiet check of the shell (useUpdateStatus): the tile and
// the second line of 关于 read it, a press on 检查更新 asks the same checker, and 更新 opens 关于 with the update sheet already open (the page reads `openUpdate`; opening it puts the update dot out).
const { t, locale } = useI18n()
const shell = inject('webShell', null)
const update = useUpdateStatus()
const hostVersion = ref('')
const installed = computed(() => update.version.value?.version || hostVersion.value)
const aboutSub = computed(() => aboutLine(installed.value, { state: update.state.value }, helpTileTexts(String(locale.value))))
onMounted(async () => {
  try { hostVersion.value = (await hostData.refresh('config'))?.version || '' } catch { /* the page shows no version */ }
  update.ensure()
})
</script>
