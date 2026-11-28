<script setup>
import { computed, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { MButton, MDialog, MDisplayCards, MExpander, MFooter, MGroup, MInfo, MItem, MModeCard, MPrepSeg, MSection, MSeg, MSnackbar, MSwitchItem, displayTreeTexts, vddOn } from 'foundation-ui/vue'
import { vddPhysicalDisplays } from 'foundation-ui/settings/index.js'
import { $tp } from '../../../platform-i18n'
import CfgExpert from '../../../components/cfg/CfgExpert.vue'
import CfgField from '../../../components/cfg/CfgField.vue'
import CfgSwitch from '../../../components/cfg/CfgSwitch.vue'
import { useVddSettings } from '../../../composables/useVddSettings.js'
import { prepChoices } from '../../../composables/displayPrep.js'
import { gdiNameOf, modeTargetOf, SCALES, useDisplayLive } from '../../../composables/useDisplayLive.js'

// 显示器: the output display (cards like Windows' display settings, 自动 first), the display mode of the display that is streamed (three sliders, applied live by the control panel),
// how the physical displays are handled during a stream, and the headless switch. The physical display names and ids are in the 高级 expander.
const props = defineProps({ platform: { type: String, default: 'windows' }, config: { type: Object, required: true }, session: { type: Object, default: null }, focus: { type: String, default: '' } })
const { t, locale } = useI18n()
const x = computed(() => displayTreeTexts(locale.value))
const store = useVddSettings()
const windows = computed(() => props.platform === 'windows')

const VDD = 'ZakoHDR'
const physical = computed(() => (props.config.display_devices || []).filter((d) => !/FRIENDLY NAME:\s*Zako HDR/i.test(String(d?.data || ''))).map((d) => {
  const [info] = vddPhysicalDisplays({ display_devices: [d] })
  return { id: d.device_id, name: info?.name || String(d.device_id), gdi: gdiNameOf(d.data), primary: /DEVICE STATE:\s*PRIMARY/i.test(String(d.data || '')) }
}))
const chosen = computed({
  get: () => (props.config.output_name === VDD ? 'virtual' : props.config.output_name ? props.config.output_name : 'auto'),
  set: (id) => { props.config.output_name = id === 'virtual' ? VDD : id === 'auto' ? '' : id },
})
// The virtual display is the streamed target, but Windows applies its mode and scale through the physical display that is primary underneath it.
const modeTarget = computed(() => modeTargetOf(chosen.value, physical.value))
const target = computed(() => modeTarget.value?.gdi || '')
const live = useDisplayLive(() => target.value)
const { model: modeModel, resolutions: modeResolutions, refreshes: modeRefreshes, state: modeState, error: modeError } = live
live.load()
const streamed = computed(() => !!props.session)
const targetName = computed(() => modeTarget.value?.name || (chosen.value === 'auto' || chosen.value === 'virtual' ? x.value.display.auto : ''))
const sizeOf = (id) => (id === chosen.value && modeModel.value.resolution ? modeModel.value.resolution : '')
const cards = computed(() => [
  { id: 'auto', kind: 'auto', title: x.value.display.auto, text: x.value.display.autoText, size: sizeOf('auto') },
  ...(windows.value ? [{ id: 'virtual', kind: 'virtual', title: x.value.display.virtual, text: store.summary.value.active === null ? '' : store.summary.value.active ? x.value.display.virtualOn : x.value.display.virtualOff, size: sizeOf('virtual') }] : []),
  ...physical.value.map((d) => ({ id: d.id, kind: 'physical', title: d.name, text: d.gdi.replace(/^\\\\\.\\/, ''), primary: d.primary, size: sizeOf(d.id) })),
])

const prepOptions = computed(() => prepChoices({ windows: windows.value, virtual: chosen.value === 'virtual', current: props.config.display_device_prep }).map((value) => ({ value, label: x.value.prep[value] })))
const prepModel = computed({ get: () => props.config.display_device_prep || 'no_operation', set: (value) => { props.config.display_device_prep = value } })
const prepEffect = computed(() => x.value.prep.effect?.[prepModel.value] || '')
// 恢复机制说明挂在【显示器配置】标题旁的 info 图标上（MSection description），改动布局的策略才显示
const prepRestore = computed(() => (prepModel.value === 'no_operation' ? '' : t('config.prep_restore')))
// 息屏串流 (streaming with the physical screen off) needs the virtual display as the streamed output,
// 仅使用此显示器 as the layout and the whole-display capture; all three together are the ready state.
const screenoffReady = computed(() => chosen.value === 'virtual' && prepModel.value === 'ensure_only_display' && (props.config.capture_target || 'display') === 'display')
const screenoffAsk = ref(false)
const screenoffSnack = ref(false)
// 捕获目标: an experimental choice parked in the page's 高级 expander; window capture needs Windows
// Graphics Capture, so the choice also selects the capture method.
const targets = computed(() => ['display', 'window'].map((value) => ({ value, label: t(`config.capture_target_${value}`) })))
const captureTarget = computed({ get: () => props.config.capture_target || 'display', set: (value) => { props.config.capture_target = value } })
watch(() => props.config.capture_target, (value) => { if (value === 'window' && props.config.capture !== 'wgc') props.config.capture = 'wgc' })
const screenoffOutputNow = computed(() => (chosen.value === 'virtual' ? x.value.display.virtual : chosen.value === 'auto' ? x.value.display.auto : physical.value.find((d) => d.id === chosen.value)?.name || chosen.value))
const screenoffCaptureNow = computed(() => t(`config.capture_target_${captureTarget.value}`))
// the wizard writes through the reactive setters (chosen/prepModel/captureTarget), so the change lands in the
// same unsaved-changes flow as a manual edit; nothing is saved or applied here
const applyScreenoff = () => {
  chosen.value = 'virtual'
  prepModel.value = 'ensure_only_display'
  if ((props.config.capture_target || 'display') !== 'display') captureTarget.value = 'display'
  screenoffAsk.value = false
  screenoffSnack.value = true
}

const headless = computed({ get: () => vddOn(props.config?.vdd_headless_create), set: (value) => store.set({ vdd_headless_create: value }) })
const open = ref(props.focus === 'display_devices')
</script>

<template>
  <MSection :title="x.sections.output.target">
    <template v-if="windows">
      <MDisplayCards v-model="chosen" data-cfg="output_name" :label="$t('config.output_name_windows')" :items="cards" :primary-label="x.display.primary" />
      <MFooter v-if="!physical.length">{{ x.display.noneFound }}</MFooter>
    </template>
    <MGroup v-else>
      <CfgField :config="config" k="output_name" label="config.output_name_unix" help="config.output_name_desc_unix" placeholder="0" />
    </MGroup>
  </MSection>

  <MSection v-if="windows" :title="x.sections.output.prep" :description="prepRestore">
    <MPrepSeg v-model="prepModel" data-cfg="display_device_prep" :label="$tp('config.display_device_prep')" :options="prepOptions" />
    <MFooter v-if="prepEffect">{{ t('config.prep_effect_prefix', { effect: prepEffect }) }}</MFooter>
    <MGroup>
      <CfgSwitch :config="config" k="stop_on_last_video_session" :on="true" :off="false" def="false" icon="logout" />
    </MGroup>
    <MGroup>
      <MItem icon="dark_mode" wrap :title="t('config.screenoff_card_title')" :text="screenoffReady ? t('config.screenoff_status_ready') : t('config.screenoff_status_todo')">
        <template #trail>
          <MInfo :text="screenoffReady ? t('config.screenoff_ready_text') : t('config.screenoff_todo_text')" />
          <MButton v-if="!screenoffReady" variant="tonal" size="s" @click="screenoffAsk = true">{{ t('config.screenoff_action') }}</MButton>
        </template>
      </MItem>
    </MGroup>
  </MSection>

  <MSection :title="x.sections.output.mode">
    <MModeCard
      v-model="modeModel" :resolutions="modeResolutions" :refreshes="modeRefreshes" :scales="SCALES" :target="targetName" :streaming="streamed"
      :live="live.live" :state="modeState" :error="modeError" :locale="locale" data-cfg="display_mode" @apply="live.apply"
    />
    <MGroup>
      <CfgSwitch :config="config" k="variable_refresh_rate" def="disabled" />
    </MGroup>
  </MSection>

  <MSection v-if="windows" :title="x.sections.output.headless">
    <MGroup>
      <MSwitchItem v-model="headless" data-cfg="vdd_headless_create" icon="desktop_access_disabled" :title="$t('config.vdd_headless_create')" :text="x.headless.text" :disabled="!store.canSaveProtected || !!store.busy.vdd_headless_create" />
    </MGroup>
  </MSection>

  <MExpander v-if="windows" v-model:open="open" :title="x.sections.output.advanced" :text="physical.length ? (physical.length === 1 ? x.display.countOne : x.display.countMany).replace('{n}', physical.length) : ''">
    <MGroup>
      <MItem data-cfg="capture_target" :title="$t('config.capture_target')" icon="tab" stack>
        <template #trail><span class="m-item__count">{{ t('config.experimental') }}</span></template>
        <template #control><MSeg v-model="captureTarget" block :label="$t('config.capture_target')" :options="targets" /></template>
      </MItem>
      <CfgField v-if="config.capture_target === 'window'" :config="config" k="window_title" :placeholder="x.display.windowPlaceholder" />
      <MFooter v-if="config.capture_target === 'window'">{{ x.display.windowText }}</MFooter>
      <MItem v-for="d in physical" :key="d.id" icon="monitor" :title="d.name" :text="d.id" plain />
      <MItem v-if="!physical.length" icon="monitor" :title="x.display.noneFound" plain />
    </MGroup>
  </MExpander>

  <MDialog v-model="screenoffAsk" :title="t('config.screenoff_dialog_title')" icon="dark_mode" tone="info">
    <p class="screenoff-wizard__lead">{{ t('config.screenoff_dialog_body') }}</p>
    <div class="screenoff-wizard__rows">
      <p v-if="chosen !== 'virtual'" class="screenoff-wizard__row">
        <span>{{ t('config.output_name_windows') }}</span>
        <span>{{ screenoffOutputNow }} → {{ x.display.virtual }}</span>
      </p>
      <p v-if="prepModel !== 'ensure_only_display'" class="screenoff-wizard__row">
        <span>{{ t('config.screenoff_dialog_prep') }}</span>
        <span>{{ x.prep[prepModel] }} → {{ x.prep.ensure_only_display }}</span>
      </p>
      <p v-if="(props.config.capture_target || 'display') !== 'display'" class="screenoff-wizard__row">
        <span>{{ t('config.capture_target') }}</span>
        <span>{{ screenoffCaptureNow }} → {{ t('config.capture_target_display') }}</span>
      </p>
    </div>
    <p class="screenoff-wizard__risk">{{ t('config.screenoff_dialog_risk') }}</p>
    <template #actions>
      <MButton variant="text" @click="screenoffAsk = false">{{ t('config.screenoff_dialog_cancel') }}</MButton>
      <MButton @click="applyScreenoff">{{ t('config.screenoff_dialog_confirm') }}</MButton>
    </template>
  </MDialog>
  <MSnackbar v-model="screenoffSnack" :message="t('config.screenoff_applied')" />
</template>

<style scoped>
.screenoff-wizard__lead { margin: 0 0 12px; }
.screenoff-wizard__rows { display: grid; gap: 8px; margin: 0 0 12px; }
.screenoff-wizard__row { display: flex; justify-content: space-between; gap: 12px; margin: 0; font: var(--m-body-medium); }
.screenoff-wizard__row span:first-child { color: var(--md-sys-color-on-surface-variant); }
.screenoff-wizard__risk { margin: 0; color: var(--md-sys-color-on-surface-variant); font: var(--m-body-small); }
</style>
