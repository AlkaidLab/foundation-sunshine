<script setup>
import { computed, ref, onMounted, onBeforeUnmount } from 'vue'
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
function toggle(event) {
  props.config.nvenc_vdd_force_vsync = event.target.checked ? 'enabled' : 'disabled'
}
</script>

<template>
  <div v-if="view.visible" class="mt-3">
    <div class="form-check form-switch">
      <input id="nvenc_vdd_force_vsync" type="checkbox" role="switch" class="form-check-input"
             :checked="view.checked" :disabled="view.disabled" @change="toggle"
             aria-describedby="nvenc_vdd_force_vsync_desc nvenc_vdd_force_vsync_status" />
      <label class="form-check-label" for="nvenc_vdd_force_vsync">{{ $t('config.nvenc_vdd_force_vsync') }}</label>
    </div>
    <div id="nvenc_vdd_force_vsync_desc" class="form-text">{{ $t('config.nvenc_vdd_force_vsync_desc') }}</div>
    <div id="nvenc_vdd_force_vsync_status" class="form-text" role="status">
      {{ $t(`config.nvenc_vdd_force_vsync_${view.state}`) }}
    </div>
    <a v-if="view.state === 'unmanaged'" href="#nv" class="small">{{ $t('config.nvenc_manage_driver_settings') }}</a>
  </div>
</template>
