<script setup>
import { computed, h, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { MBanner, MButton, MChoiceCards, MExpander, MFooter, MGroup, MIcon, MInfo, MPageLoading, MSection, displayTreeTexts } from 'foundation-ui/vue'
import { lazyPane } from '../../../utils/lazyPane.js'
import BitrateControl from '../audiovideo/BitrateControl.vue'
import EncoderMatrix from '../encoders/EncoderMatrix.vue'
import { useSettingsLive } from '../../../composables/useSettingsLive.js'
import { useCodecFormat } from '../../../composables/useCodecFormat.js'
import { getPreferredEncoderTab } from '../../../config/encoderTabs.js'
import { encoderLabel } from '../../../utils/shellLive.js'

// 编码与码率: the bitrate cap, the video format as one choice (auto, H.264, HEVC, AV1: the host's codec_strategy / hevc_mode / av1_mode), the encoder as choice cards, what is in effect as
// read-only pills, and, in the 高级 expander, the parameters of the encoder that is in use: only that vendor's (the others are one choice away).
const EncoderLoading = () => h(MPageLoading, { groups: 2 })
const encoderChunk = (loader) => lazyPane(loader, { loading: EncoderLoading })
const NvidiaNvencEncoder = encoderChunk(() => import('../encoders/NvidiaNvencEncoder.vue'))
const IntelQuickSyncEncoder = encoderChunk(() => import('../encoders/IntelQuickSyncEncoder.vue'))
const AmdAmfEncoder = encoderChunk(() => import('../encoders/AmdAmfEncoder.vue'))
const VideotoolboxEncoder = encoderChunk(() => import('../encoders/VideotoolboxEncoder.vue'))
const SoftwareEncoder = encoderChunk(() => import('../encoders/SoftwareEncoder.vue'))

const props = defineProps(['platform', 'config', 'encoders', 'focus'])
const { t, locale } = useI18n()
const x = computed(() => displayTreeTexts(locale.value))
const live = useSettingsLive({ platform: props.platform, want: ['session', 'cpu', 'gpu'] })
const { format, hdr } = useCodecFormat(props.config)

const COMPONENTS = { nv: NvidiaNvencEncoder, qsv: IntelQuickSyncEncoder, amd: AmdAmfEncoder, vt: VideotoolboxEncoder, sw: SoftwareEncoder }
const VALUE = { nv: 'nvenc', qsv: 'quicksync', amd: 'amdvce', vt: 'videotoolbox', sw: 'software' }
const NAME = { nv: 'NVENC', qsv: 'Quick Sync', amd: 'AMF', vt: 'VideoToolbox', sw: '' }
const VENDOR = { nv: 'NVIDIA', qsv: 'Intel', amd: 'AMD', vt: 'Apple', sw: 'CPU' }
const ICON = { auto: 'bolt', nv: 'memory', qsv: 'memory', amd: 'memory', vt: 'videocam', sw: 'developer_board' }
const CARD = { nv: /nvidia|geforce|rtx|gtx|quadro/i, qsv: /intel/i, amd: /amd|radeon/i }
const ids = computed(() => (props.encoders || []).map((e) => e.id))

// the video format
const formatCards = computed(() => ['auto', 'h264', 'hevc', 'av1'].map((value) => ({ value, label: x.value.encoding.formats[value], text: x.value.encoding.formatText[value] })))

// the encoder
const adapters = computed(() => (Array.isArray(props.config.adapters) ? props.config.adapters.map((a) => String(a?.name || '')) : []))
const missing = (id) => adapters.value.length > 0 && CARD[id] && !adapters.value.some((n) => CARD[id].test(n))
const encoderCards = computed(() => [
  { value: '', label: x.value.encoding.encoderAuto, text: x.value.encoding.encoderAutoText, icon: ICON.auto },
  ...(props.encoders || []).map((e) => ({ value: VALUE[e.id] ?? e.id, label: NAME[e.id] || t('config.encoder_software'), text: missing(e.id) ? x.value.encoding.encoderMissing.replace('{vendor}', VENDOR[e.id]) : VENDOR[e.id] || '', icon: ICON[e.id] || 'memory', disabled: missing(e.id) })),
])
const encoder = computed({ get: () => String(props.config.encoder ?? ''), set: (value) => { props.config.encoder = value; viewing.value = '' } })

const preferred = computed(() => getPreferredEncoderTab({
  activeEncoder: props.config?.active_encoder, configuredEncoder: props.config?.encoder, selectedAdapter: props.config?.adapter_name, adapters: props.config?.adapters, platform: props.platform, availableTabIds: ids.value,
}))
const chosenTab = computed(() => ids.value.find((id) => VALUE[id] === String(props.config?.encoder ?? '')) || '')
// a search hit or a link can ask to see the parameters of an encoder that is not the chosen one: shown with a note and a way to use it
const viewing = ref('')
const encoderForFocus = (focus) => {
  if (focus === 'encoder') return ''
  const prefix = { nv: 'nvenc_', qsv: 'qsv_', amd: 'amd_', vt: 'vt_', sw: 'sw_' }
  return Object.entries(prefix).find(([, p]) => String(focus || '').startsWith(p))?.[0] || ''
}
watch(() => props.focus, (focus) => { viewing.value = encoderForFocus(focus); if (viewing.value) open.value = true }, { immediate: true })
const shown = computed(() => (viewing.value && viewing.value !== chosenTab.value ? viewing.value : chosenTab.value || preferred.value))
const useViewed = () => { props.config.encoder = VALUE[viewing.value]; viewing.value = '' }
const open = ref(!!encoderForFocus(props.focus))
const vendorName = computed(() => (shown.value ? NAME[shown.value] || encoderLabel(props.config?.active_encoder) || t(`tabs.${shown.value}`) : ''))
</script>

<template>
  <MSection :title="x.sections.encoders.bitrate">
    <MGroup><BitrateControl :config="config" /></MGroup>
  </MSection>

  <MSection :title="x.sections.encoders.format">
    <MChoiceCards v-model="format" class="cfg-format-cards" data-cfg="codec_strategy" :label="$t('config.codec_strategy')" :options="formatCards">
      <template #art="{ option }"><span class="cfg-format-art" :class="`is-${option.value}`">{{ option.value === 'auto' ? 'AUTO' : option.label }}</span></template>
    </MChoiceCards>
    <span class="cfg-anchor" data-cfg="hevc_mode" :label="$t('config.hevc_mode')" />
    <span class="cfg-anchor" data-cfg="av1_mode" :label="$t('config.av1_mode')" />
  </MSection>

  <MSection :title="x.sections.encoders.encoder">
    <template #action><MInfo :text="$t('config.encoder_desc')" /></template>
    <MChoiceCards v-model="encoder" class="cfg-encoder-cards" data-cfg="encoder" :label="$t('config.encoder')" :options="encoderCards" />
  </MSection>

  <MSection :title="x.sections.encoders.effective">
    <EncoderMatrix :config="config" :session="live.sessions.value[0]" :system="live.system.value" />
  </MSection>

  <MExpander v-if="shown && COMPONENTS[shown]" v-model:open="open" :title="x.sections.encoders.advanced" :text="vendorName">
    <MBanner v-if="viewing && viewing !== chosenTab" tone="info" icon="visibility">
      {{ $t('config.encoder_view_note', { name: $t(`tabs.${viewing}`) }) }}
      <template #actions><MButton variant="text" size="sm" @click="useViewed">{{ $t('config.encoder_use_this') }}</MButton></template>
    </MBanner>
    <component :is="COMPONENTS[shown]" :key="shown" :platform="platform" :config="config" />
  </MExpander>
  <MFooter v-else-if="!chosenTab">{{ $t('config.encoder_auto_none') }}</MFooter>
</template>

<style>
.cfg-format-art { font: 700 18px/1 var(--md-ref-typeface-brand, sans-serif); letter-spacing: .02em; color: color-mix(in srgb, var(--md-sys-color-primary) 62%, var(--md-sys-color-on-surface)); }
.cfg-encoder-cards.m-cards { grid-template-columns: repeat(auto-fit, minmax(min(100%, 120px), 1fr)); }
.cfg-format-cards.m-cards { grid-template-columns: repeat(auto-fit, minmax(min(100%, 120px), 1fr)); }
.cfg-encoder-cards .m-cards__text, .cfg-format-cards .m-cards__text, .cfg-encoder-cards .m-cards__label, .cfg-format-cards .m-cards__label { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.cfg-anchor { display: block; height: 0; overflow: hidden; }
</style>
