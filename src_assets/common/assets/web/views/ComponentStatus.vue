<template>
  <div class="m-app m-root component-status-page">
    <MComponentStatus :status="status" />
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
import { onBeforeUnmount, ref } from 'vue'
import { MComponentStatus } from 'foundation-ui/vue'
import VersionCard from '../components/common/VersionCard.vue'
import { useUpdateStatus } from '../composables/useUpdateStatus.js'
import { useComponentStatus } from '../composables/useComponentStatus.js'

// 组件状态: the shared page. Its controller is the one the shell registered (the home cards read the same one); the host row's update
// action opens the same update sheet as the about page.
const update = useUpdateStatus()
const sheet = ref(false)
const status = useComponentStatus()
status.hooks.openUpdate = () => { sheet.value = true }
onBeforeUnmount(() => { status.hooks.openUpdate = null })
</script>
