<script setup>
import { computed, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { MDisplayCards, MExpander, MFooter, MGroup, MItem, MModeCard, MPrepSeg, MSection, MSeg, MSwitchItem, displayTreeTexts } from 'foundation-ui/vue'
import { vddPhysicalDisplays } from 'foundation-ui/settings/index.js'
import { $tp } from '../../../platform-i18n'
import CfgExpert from '../../../components/cfg/CfgExpert.vue'
import CfgField from '../../../components/cfg/CfgField.vue'
import CfgSwitch from '../../../components/cfg/CfgSwitch.vue'
import { useVddSettings } from '../../../composables/useVddSettings.js'
import { gdiNameOf, SCALES, useDisplayLive } from '../../../composables/useDisplayLive.js'

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
// the display the mode card applies to: the chosen physical display, else the virtual one while it exists, else the main display
const target = computed(() => physical.value.find((d) => d.id === chosen.value)?.gdi || '')
const live = useDisplayLive(() => target.value)
const { model: modeModel, resolutions: modeResolutions, refreshes: modeRefreshes, state: modeState, error: modeError } = live
live.load()
const streamed = computed(() => !!props.session)
const targetName = computed(() => (chosen.value === 'virtual' ? x.value.display.virtual : physical.value.find((d) => d.id === chosen.value)?.name || (chosen.value === 'auto' ? x.value.display.auto : '')))
const sizeOf = (id) => (id === chosen.value && modeModel.value.resolution ? modeModel.value.resolution : '')
const cards = computed(() => [
  { id: 'auto', kind: 'auto', title: x.value.display.auto, text: x.value.display.autoText, size: sizeOf('auto') },
  ...(windows.value ? [{ id: 'virtual', kind: 'virtual', title: x.value.display.virtual, text: store.summary.value.active === null ? '' : store.summary.value.active ? x.value.display.virtualOn : x.value.display.virtualOff, size: sizeOf('virtual') }] : []),
  ...physical.value.map((d) => ({ id: d.id, kind: 'physical', title: d.name, text: d.gdi.replace(/^\\\\\.\\/, ''), primary: d.primary, size: sizeOf(d.id) })),
])

// window capture: choosing a window needs Windows Graphics Capture, so the choice also selects it
const targets = computed(() => ['display', 'window'].map((value) => ({ value, label: t(`config.capture_target_${value}`) })))
watch(() => props.config.capture_target, (value) => { if (value === 'window' && props.config.capture !== 'wgc') props.config.capture = 'wgc' })

const captureTarget = computed({ get: () => props.config.capture_target || 'display', set: (value) => { props.config.capture_target = value } })
const prepOptions = computed(() => ['no_operation', 'ensure_active', 'ensure_primary', 'ensure_only_display'].map((value) => ({ value, label: x.value.prep[value] })))
const prepModel = computed({ get: () => props.config.display_device_prep || 'no_operation', set: (value) => { props.config.display_device_prep = value } })
const prepEffect = computed(() => x.value.prep.effect?.[prepModel.value] || '')
const prepExtra = computed(() => (props.config.display_device_prep === 'ensure_secondary' ? [{ value: 'ensure_secondary', label: x.value.prep.ensure_secondary }] : []))

const headless = computed({ get: () => store.summary.value.headless || (String(props.config.vdd_headless_create) === 'enabled' || props.config.vdd_headless_create === true), set: (value) => store.set({ vdd_headless_create: value }) })
const open = ref(props.focus === 'display_devices')
</script>

<template>
  <MSection :title="x.sections.output.target">
    <template v-if="windows">
      <MDisplayCards v-model="chosen" data-cfg="output_name" :label="$t('config.output_name_windows')" :items="cards" :primary-label="x.display.primary" />
      <MFooter v-if="!physical.length">{{ x.display.noneFound }}</MFooter>
      <MGroup>
        <MItem data-cfg="capture_target" :title="$t('config.capture_target')" icon="tab" stack>
          <template #control><MSeg v-model="captureTarget" block :label="$t('config.capture_target')" :options="targets" /></template>
        </MItem>
        <CfgField v-if="config.capture_target === 'window'" :config="config" k="window_title" :placeholder="x.display.windowPlaceholder" />
      </MGroup>
      <MFooter v-if="config.capture_target === 'window'">{{ x.display.windowText }}</MFooter>
    </template>
    <MGroup v-else>
      <CfgField :config="config" k="output_name" label="config.output_name_unix" help="config.output_name_desc_unix" placeholder="0" />
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

  <MSection v-if="windows" :title="x.sections.output.prep">
    <MPrepSeg v-model="prepModel" data-cfg="display_device_prep" :label="$tp('config.display_device_prep')" :options="[...prepOptions, ...prepExtra]" />
    <MFooter v-if="prepEffect">{{ prepEffect }}</MFooter>
    <MFooter v-if="prepModel !== 'no_operation'">{{ x.prep.revert }}</MFooter>
  </MSection>

  <MSection v-if="windows" :title="x.sections.output.headless">
    <MGroup>
      <MSwitchItem v-model="headless" data-cfg="vdd_headless_create" icon="desktop_access_disabled" :title="$t('config.vdd_headless_create')" :text="x.headless.text" :disabled="!store.canSaveProtected || !!store.busy.vdd_headless_create" />
    </MGroup>
  </MSection>

  <MExpander v-if="windows" v-model:open="open" :title="x.sections.output.advanced" :text="physical.length ? (physical.length === 1 ? x.display.countOne : x.display.countMany).replace('{n}', physical.length) : ''">
    <MGroup>
      <MItem v-for="d in physical" :key="d.id" icon="monitor" :title="d.name" :text="d.id" plain />
      <MItem v-if="!physical.length" icon="monitor" :title="x.display.noneFound" plain />
    </MGroup>
  </MExpander>
</template>
