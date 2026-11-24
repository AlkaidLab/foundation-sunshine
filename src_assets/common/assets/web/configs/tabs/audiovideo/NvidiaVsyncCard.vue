<script setup>
import { computed, ref, onMounted, onBeforeUnmount } from 'vue'
import { MItem, MSwitchItem } from 'foundation-ui/vue'
import { apiJson } from '../../../utils/apiFetch.js'
import { nvidiaVsyncUi, observeNvidiaVsyncStatus } from '../../../utils/nvidiaVsync.js'

const props = defineProps({ config: Object, platform: String })
const capability = ref(props.config.nvidia_vsync)
const view = computed(() => nvidiaVsyncUi({ ...props.config, nvidia_vsync: capability.value }, props.platform))
let stopStatus = () => {}
onMounted(() => {
  stopStatus = observeNvidiaVsyncStatus({
    load: options => apiJson('/api/config', options),
    onStatus: data => { capability.value = data.nvidia_vsync },
    onError: () => { capability.value = { ...capability.value, detection_known: false, state: 'unknown' } },
  })
})
onBeforeUnmount(() => stopStatus())
function toggle(on) {
  props.config.nvenc_vdd_force_vsync = on ? 'enabled' : 'disabled'
}
</script>

<template>
  <template v-if="view.visible">
    <MSwitchItem :model-value="view.checked" data-cfg="nvenc_vdd_force_vsync" :title="$t('config.nvenc_vdd_force_vsync')" :info="$t('config.nvenc_vdd_force_vsync_desc')" :disabled="view.disabled" @update:model-value="toggle" />
    <MItem icon="info" :title="$t(`config.nvenc_vdd_force_vsync_${view.state}`)" :href="view.state === 'unmanaged' ? '#nv' : ''" :text="view.state === 'unmanaged' ? $t('config.nvenc_manage_driver_settings') : ''" />
  </template>
</template>
