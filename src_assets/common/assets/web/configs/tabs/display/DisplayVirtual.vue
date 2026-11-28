<script setup>
import { computed, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { MExpander, MFooter, MGroup, MItem, MSection, MSwitchItem, MVddDevice, displayTreeTexts, vddOn } from 'foundation-ui/vue'
import { settingsHash } from 'foundation-ui/settings/index.js'
import CfgSwitch from '../../../components/cfg/CfgSwitch.vue'
import { useVddSettings } from '../../../composables/useVddSettings.js'
import { useVddStatus } from '../../../composables/useVddStatus.js'
import { writeTrayVddAction } from '../../../composables/trayVddAction.js'
import { edidResolutionOptions } from '../../../utils/vddEdidResolution.js'

// 基地虚拟显示器: the device card (the driver, whether the display exists, keep enabled) and the way to the settings that belong to it. Nothing here is a second copy of a setting: headless is
// the same switch as on 显示器, and the modes, the calibration and the scaling are links.
const props = defineProps({ config: { type: Object, required: true }, platform: { type: String, default: 'windows' }, resolutions: { type: Array, required: true }, fps: { type: Array, required: true }, focus: { type: String, default: '' } })
const { locale } = useI18n()
const x = computed(() => displayTreeTexts(locale.value))
const store = useVddSettings()
const status = useVddStatus()
const busy = ref(false)
const advancedOpen = ref(props.focus === 'vdd_edid_native_resolution')
watch(() => props.focus, value => { if (value === 'vdd_edid_native_resolution') advancedOpen.value = true })
const edidOptions = computed(() => edidResolutionOptions(props.resolutions, props.fps, props.config.vdd_edid_native_resolution))
const invalidEdidSelection = computed(() => edidOptions.value.some(option => option.value === props.config.vdd_edid_native_resolution && option.reason))
const state = computed(() => {
  if (status.vddStatusLoading.value && status.vddStatus.value.state === 'unknown') return 'checking'
  const s = status.vddStatus.value.state
  if (s === 'degraded') return 'degraded'
  if (status.vddReady.value) return 'ready'
  return s === 'unknown' ? 'unknown' : 'missing'
})
const keep = computed({ get: () => vddOn(store.config.value?.vdd_keep_enabled), set: (value) => store.set({ vdd_keep_enabled: value }) })
const headless = computed({ get: () => vddOn(store.config.value?.vdd_headless_create), set: (value) => store.set({ vdd_headless_create: value }) })
async function act(action) {
  busy.value = true
  try { await writeTrayVddAction(action); await store.refresh() } catch { /* the store keeps the host's refusal */ } finally { busy.value = false }
}
async function install() { try { busy.value = true; await status.installVdd() } catch { /* the composable keeps the error */ } finally { busy.value = false; await store.refresh() } }
onMounted(() => { status.refreshVddStatus(); store.refresh() })
const go = (sub) => { window.location.hash = settingsHash({ tab: 'display', sub }) }
</script>

<template>
  <MSection :title="x.sections.virtual.device">
    <MVddDevice
      v-model:keep="keep" data-cfg="vdd_keep_enabled" :label="$t('config.vdd_keep_enabled')" :state="state" :installed-version="status.vddStatus.value.installed_version || ''" :bundled-version="status.vddStatus.value.bundled_version || ''"
      :active="store.summary.value.active" :busy="busy" :can-install="status.canManageVdd.value" :can-keep="store.canSaveProtected" :keep-busy="!!store.busy.vdd_keep_enabled"
      :status-text="status.vddStatus.value.status_text" :locale="locale" @install="install" @recheck="status.refreshVddStatus" @create="act('vdd_create')" @close="act('vdd_destroy')"
    />
    <MGroup>
      <MSwitchItem v-model="headless" icon="desktop_access_disabled" :title="x.device.headless" :disabled="!store.canSaveProtected || !!store.busy.vdd_headless_create" />
      <CfgSwitch :config="config" k="vdd_reuse" def="disabled" />
    </MGroup>
  </MSection>
  <MSection :title="x.sections.virtual.links">
    <MGroup>
      <MItem action icon="aspect_ratio" :title="x.device.links.modes" @click="go('modes')" />
      <MItem action icon="hdr_on" :title="x.device.links.calibration" @click="go('hdr')" />
      <MItem action icon="format_size" :title="x.device.links.scaling" @click="go('output')" />
    </MGroup>
  </MSection>
  <MExpander v-model:open="advancedOpen" :title="$t('shell.display_advanced')">
    <MGroup>
      <MItem data-cfg="vdd_edid_native_resolution" :title="$t('config.vdd_edid_native_resolution')" :text="$t('config.vdd_edid_native_resolution_desc')" stack>
        <template #control>
          <select id="vdd_edid_native_resolution" v-model="config.vdd_edid_native_resolution" class="edid-native-select" :aria-label="$t('config.vdd_edid_native_resolution')" aria-describedby="vdd_edid_native_resolution_hint">
            <option v-for="option in edidOptions" :key="option.value" :value="option.value" :disabled="!!option.reason">
              {{ option.value === 'default' ? $t('config.vdd_edid_native_resolution_default') : option.value.replace('x', '×') }}{{ option.reason ? ' — ' + $t('config.vdd_edid_native_resolution_' + option.reason) : '' }}
            </option>
          </select>
        </template>
      </MItem>
    </MGroup>
    <MFooter id="vdd_edid_native_resolution_hint">{{ $t('config.vdd_edid_native_resolution_hint') }}</MFooter>
    <MFooter v-if="invalidEdidSelection" class="edid-native-error" role="alert">{{ $t('config.vdd_edid_native_resolution_invalid') }}</MFooter>
  </MExpander>
</template>

<style scoped>
.edid-native-select { width: 100%; font: inherit; padding: .7rem; border: 1px solid var(--md-sys-color-outline); border-radius: 12px; color: var(--md-sys-color-on-surface); background: var(--md-sys-color-surface-container); }
.edid-native-error { color: var(--md-sys-color-error); }
</style>
