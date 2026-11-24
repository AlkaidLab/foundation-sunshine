<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { MExpander, MFooter, MGroup, MHdrStatus, MItem, MSection, MSwitchItem, displayTreeTexts } from 'foundation-ui/vue'
import CfgChoice from '../../../components/cfg/CfgChoice.vue'
import DisplayDeviceOptions from '../audiovideo/DisplayDeviceOptions.vue'
import VulkanHdrBridgeCard from '../audiovideo/VulkanHdrBridgeCard.vue'
import { useHdrRuntime } from '../../../composables/useHdrRuntime.js'
import { useCodecFormat } from '../../../composables/useCodecFormat.js'
import { createNativeRtxHdrBridge } from '../../../utils/nativeRtxHdrBridge.js'
import { $tp } from '../../../platform-i18n'

// HDR: one status card with the switch that allows HDR; the system HDR that is turned on while streaming; the calibration of the virtual and the physical display (the control
// panel's own tools); and, in the 高级 expander, RTX HDR, the Vulkan HDR bridge and the luminance analysis.
const props = defineProps({ platform: { type: String, default: 'windows' }, config: { type: Object, required: true }, focus: { type: String, default: '' } })
const { locale } = useI18n()
const x = computed(() => displayTreeTexts(locale.value))
const runtime = useHdrRuntime({ platform: props.platform, config: props.config })
const { hdr, hdrAvailable } = useCodecFormat(props.config)
const hdrPrep = computed({ get: () => (props.config.hdr_prep || 'automatic') === 'automatic', set: (value) => { props.config.hdr_prep = value ? 'automatic' : 'no_operation' } })
const open = ref(['hdr_enhanced', 'vdd_vulkan_hdr_bridge', 'hdr_luminance_analysis'].includes(props.focus))

// calibration and RTX HDR are tools of the control panel: its frame answers the commands and the message the capture page used
const invoke = typeof window !== 'undefined' ? window.__TAURI__?.core?.invoke : null
const inFrame = typeof window !== 'undefined' && window.parent !== window
const calibrating = ref(false)
const calibrationError = ref('')
async function calibrate() {
  if (!invoke) return
  calibrating.value = true
  calibrationError.value = ''
  try { const result = await invoke('launch_windows_hdr_calibration'); if (result && result.success === false) throw new Error(result.message || 'failed') } catch (e) { calibrationError.value = String(e?.message || e) } finally { calibrating.value = false }
}
let bridge
const manager = ref(false)
const openRtx = () => { if (manager.value) bridge?.openManager(); else if (inFrame) window.parent.postMessage({ type: 'foundation-open', page: 'hdr-enhanced' }, '*') }
onMounted(() => {
  bridge = createNativeRtxHdrBridge({ windowObject: window, documentObject: document, onAvailability: (available) => { manager.value = available } })
  window.addEventListener('message', bridge.handleMessage)
  bridge.requestContext()
})
onBeforeUnmount(() => { if (bridge) window.removeEventListener('message', bridge.handleMessage) })
</script>

<template>
  <MSection :title="x.sections.hdr.status">
    <MHdrStatus v-model="hdr" data-cfg="codec_enable_hdr" :label="$t('config.codec_enable_hdr')" :disabled="!hdrAvailable" :state="runtime.view.value.state" :reason="runtime.view.value.reason" :locale="locale" />
  </MSection>

  <MSection :title="x.sections.hdr.system">
    <MGroup>
      <MSwitchItem v-model="hdrPrep" data-cfg="hdr_prep" icon="hdr_on" :title="$tp('config.hdr_prep')" :text="x.hdr.systemText" />
    </MGroup>
  </MSection>

  <MSection :title="x.sections.hdr.calibration">
    <MGroup>
      <MItem action data-cfg="hdr_calibrate_virtual" icon="tv_gen" :title="x.hdr.calibrateVirtual" :text="invoke ? '' : x.hdr.calibrateHost" :disabled="!invoke || calibrating" @click="calibrate" />
      <MItem action data-cfg="hdr_calibrate_physical" icon="monitor" :title="x.hdr.calibratePhysical" :text="invoke ? '' : x.hdr.calibrateHost" :disabled="!invoke || calibrating" @click="calibrate" />
    </MGroup>
    <MFooter v-if="calibrationError">{{ calibrationError }}</MFooter>
  </MSection>

  <MExpander v-model:open="open" :title="x.sections.hdr.advanced">
    <MGroup>
      <MItem action data-cfg="hdr_enhanced" icon="wand_stars" :title="$t('config.hdr_enhanced_open_manager')" :text="manager ? '' : x.hdr.calibrateHost" :disabled="!manager && !inFrame" @click="openRtx" />
      <VulkanHdrBridgeCard :config="config" />
    </MGroup>
    <DisplayDeviceOptions :platform="platform" :config="config" />
  </MExpander>
</template>
