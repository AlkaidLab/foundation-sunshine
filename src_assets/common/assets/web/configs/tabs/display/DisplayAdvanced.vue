<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { MButton, MFooter, MGroup, MItem, MSection, displayTreeTexts } from 'foundation-ui/vue'
import { DISPLAY_TREE } from 'foundation-ui/settings/index.js'
import { SETTING_META } from 'foundation-ui/settings/index.js'
import CfgExpert from '../../../components/cfg/CfgExpert.vue'
import VddPrerequisiteNotice from '../../../components/common/VddPrerequisiteNotice.vue'
import AdapterNameSelector from '../audiovideo/AdapterNameSelector.vue'
import NvidiaVsyncCard from '../audiovideo/NvidiaVsyncCard.vue'
import MatLiveSettings from '../audiovideo/MatLiveSettings.vue'

// 高级: the settings most people never touch, each in the part it belongs to. 捕获 (the method, the frame conversion, WGC), 显卡, 兼容性, 同步与特殊, and one button that restores the default of
// every setting of this page. An expert mode of a future platform hides this place and the 高级 expanders of the other pages.
const props = defineProps({ platform: { type: String, required: true }, config: { type: Object, required: true } })
const { t, locale } = useI18n()
const x = computed(() => displayTreeTexts(locale.value))
const invoke = typeof window !== 'undefined' ? window.__TAURI__?.core?.invoke : null
const isWGC = computed(() => props.platform === 'windows' && props.config.capture === 'wgc')
const isAMD = computed(() => props.platform === 'windows' && props.config.capture === 'amd')
const isVDD = computed(() => props.platform === 'windows' && props.config.capture === 'vdd')
const captureOptions = computed(() => {
  const list = [{ value: '', label: t('_common.autodetect') }]
  if (props.platform === 'linux') list.push({ value: 'nvfbc', label: 'NvFBC' }, { value: 'wlr', label: 'wlroots' }, { value: 'kms', label: 'KMS' }, { value: 'x11', label: 'X11' })
  if (props.platform === 'windows') list.push({ value: 'ddx', label: 'Desktop Duplication API' }, { value: 'wgc', label: 'Windows Graphics Capture' }, { value: 'amd', label: `AMD Display Capture ${t('_common.beta')}` }, { value: 'vdd', label: `${x.value.advanced.vdd} · ${x.value.advanced.vddText}` })
  return list
})
const shaders = computed(() => ['auto', 'on', 'off'].map((value) => ({ value, label: t(`config.capture_compute_shader_${value}`) })))

// WGC runs in the user's session or as a service; the control panel reports which and switches it
const userMode = ref(false)
const checking = ref(false)
const checkMode = async () => {
  if (!invoke) return
  checking.value = true
  try { userMode.value = (await invoke('is_sunshine_running_in_user_mode')) === true } catch { userMode.value = false } finally { checking.value = false }
}
const toggleMode = async () => {
  if (!invoke) return
  try { await invoke('toggle_sunshine_mode'); setTimeout(checkMode, 6000); setTimeout(checkMode, 11000) } catch { /* the next check reports the unchanged mode */ }
}
onMounted(() => { if (isWGC.value) checkMode() })
watch(isWGC, (active) => { if (active) checkMode() })

// 重置本组: the default of every setting of this page
const groupKeys = DISPLAY_TREE.find((p) => p.place === 'advanced').sections.flatMap((s) => s.keys)
const done = ref(false)
function reset() {
  for (const key of groupKeys) {
    const def = SETTING_META[key]?.def
    const cur = props.config[key]
    if (typeof def === 'boolean') props.config[key] = typeof cur === 'boolean' ? def : ['true', 'false'].includes(String(cur)) ? String(def) : def ? 'enabled' : 'disabled'
    else props.config[key] = def === null || def === undefined ? '' : def
  }
  done.value = true
  setTimeout(() => { done.value = false }, 2500)
}
const openApps = () => { window.location.href = '/apps' }
</script>

<template>
  <MSection :title="x.sections.advanced.capture">
    <MGroup>
      <CfgExpert v-if="platform !== 'macos'" :config="config" k="capture" control="sheet" :options="captureOptions" def="" />
      <CfgExpert v-if="platform === 'windows'" control="seg" :config="config" k="capture_compute_shader" :options="shaders" def="auto" />
      <CfgExpert v-if="isWGC" control="switch" :config="config" k="wgc_disable_secure_desktop" :on="true" :off="false" def="false" />
      <MItem v-if="isWGC && invoke" action :icon="userMode ? 'person' : 'settings'" :title="x.advanced.wgcMode" :text="checking ? '' : userMode ? x.advanced.wgcUser : x.advanced.wgcService" @click="toggleMode">
        <template #trail><span class="m-item__count">{{ x.advanced.wgcSwitch }}</span></template>
      </MItem>
      <CfgExpert :config="config" k="minimum_fps_target" min="0" max="1000" />
    </MGroup>
    <MFooter v-if="isVDD">{{ $t('config.capture_vdd_direct_desc') }}</MFooter>
    <VddPrerequisiteNotice :active="isVDD" />
  </MSection>

  <MSection v-if="platform !== 'macos'" :title="x.sections.advanced.gpu">
    <MGroup><AdapterNameSelector :platform="platform" :config="config" /></MGroup>
    <MFooter v-if="platform === 'windows'"><span v-html="$t('config.adapter_name_desc_windows')"></span></MFooter>
    <template v-else-if="platform === 'linux'">
      <MFooter>{{ $t('config.adapter_name_desc_linux_1') }} <code>ls /dev/dri/renderD*</code> {{ $t('config.adapter_name_desc_linux_2') }}</MFooter>
      <MFooter>{{ $t('config.adapter_name_desc_linux_3') }} <i>VAProfileH264High : VAEntrypointEncSlice</i></MFooter>
    </template>
  </MSection>

  <MSection v-if="platform === 'windows'" :title="x.sections.advanced.compat">
    <MGroup>
      <MItem action icon="apps" :title="x.advanced.overrides" :text="x.advanced.overridesText" @click="openApps" />
      <MItem v-if="isAMD" icon="info" :title="x.advanced.amd" plain />
    </MGroup>
  </MSection>

  <MSection :title="x.sections.advanced.sync">
    <MGroup>
      <NvidiaVsyncCard :config="config" :platform="platform" />
    </MGroup>
    <MatLiveSettings :config="config" />
  </MSection>

  <MSection :title="x.sections.advanced.reset">
    <MGroup>
      <MItem icon="restart_alt" :title="done ? x.advanced.resetDone : x.advanced.resetText"><template #trail><MButton size="sm" variant="tonal" icon="restart_alt" @click="reset">{{ x.advanced.reset }}</MButton></template></MItem>
    </MGroup>
  </MSection>
</template>
