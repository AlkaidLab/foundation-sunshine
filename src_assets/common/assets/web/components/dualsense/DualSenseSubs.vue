<script setup>
import { computed, inject, watch, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { DS5_SUB } from 'foundation-ui/settings/index.js'
import { localizedKeyLabel, localizedOptionLabel } from '../../config/dualsenseI18n.js'
import { MButton, MChip, MFooter, MGroup, MItem, MSection, MSnackbar } from 'foundation-ui/vue'
import CfgSub from '../cfg/CfgSub.vue'
import ControlField from './ControlField.vue'
import RangeField from './RangeField.vue'
import ColorWell from './ColorWell.vue'
import DualSenseLive from './DualSenseLive.vue'
import AdvancedWarningDialog from './AdvancedWarningDialog.vue'
import { blocksOf, rowsOf } from './tuningIcons.js'
import { useDualsenseShared, useDualsenseTuning } from '../../composables/dualsenseShared.js'

// The sub-pages of the gamepad page: one per card of everyday options, one per advanced group, and the test page. An
// advanced group is only shown once the warning was confirmed (or switched off): a link, a search hit or a reload that
// lands on one asks first, and cancelling returns to the gamepad page.
const { t, locale } = useI18n()
const nav = inject('cfgNav')
const store = useDualsenseShared()
const tuning = useDualsenseTuning(t, locale.value)
const { edited, cards, advancedGroups, unavailable, advancedOpen, valueOf, setValue, onValue, resetKey, setColor, saveMessage } = store
const embedded = window.isTauri === true && window.parent !== window

const snack = ref(false)
watch(saveMessage, (text) => { snack.value = text !== '' })
tuning.load()

const warningOpen = ref(false)
watch(() => [nav.sub.value, advancedOpen.value], ([sub, open]) => {
  if (!DS5_SUB.isGroup(sub) || open) return
  if (store.warningHidden()) advancedOpen.value = true
  else warningOpen.value = true
}, { immediate: true })
const confirmWarning = (dontShowAgain) => { store.rememberWarning(dontShowAgain); warningOpen.value = false; advancedOpen.value = true }
const cancelWarning = () => { warningOpen.value = false; nav.sub.value = '' }

// the default of a key the client executes, as the client would show it
const valueText = (key) => {
  if (key.type === 'bool') return key.default === true ? t('ds5.live.on') : t('ds5.live.off')
  if (key.type === 'enum') return localizedOptionLabel(String(key.default), locale.value)
  return key.unit ? `${key.default} ${key.unit}` : String(key.default)
}
const wiredRows = (keys) => blocksOf(rowsOf(keys))
const pair = (key) => Number(edited[key.id] ?? key.default)
const colorWired = computed(() => [81, 82, 83, 84].every(store.isWired))
// the light bar colour is edited by the colour row (swatches, hex, picker), not by three channel sliders
const cardRows = (card) => (card.lighting && colorWired.value ? card.rows.filter((key) => ![82, 83, 84].includes(key.id)) : card.rows)
const testNote = ref('')
let testTimer = null
const TESTS = ['identify', 'haptics', 'trigger_left', 'trigger_right', 'stop']
const runTest = (action) => {
  testNote.value = t('ds5.test_panel.preview_only', { action: t(`ds5.test_panel.${action}`) })
  clearTimeout(testTimer)
  testTimer = setTimeout(() => { testNote.value = '' }, 1600)
}
const openTool = () => window.parent.postMessage({ type: 'foundation-open', page: 'controller-test' }, '*')
</script>

<template>
  <CfgSub v-for="card in cards" :id="DS5_SUB.card(card.id)" :key="card.id" :title="t(card.titleKey)">
    <DualSenseLive :id="card.id" />
    <MSection v-if="cardRows(card).length || (card.lighting && colorWired)">
      <MGroup>
        <ControlField v-for="key in cardRows(card)" :key="key.id" :descriptor="key" :model-value="edited[key.id]" @update:model-value="onValue(key.id, $event)" @reset="resetKey(key.id)" />
        <ColorWell v-if="card.lighting && colorWired" :r="Number(edited[82])" :g="Number(edited[83])" :b="Number(edited[84])" :mode="String(edited[81])" @update="setColor" />
      </MGroup>
    </MSection>
    <MSection v-if="card.pending.length" :title="t('ds5.pending.title')">
      <MGroup>
        <ControlField v-for="key in card.pending" :key="key.id" :descriptor="key" :model-value="edited[key.id]" disabled />
      </MGroup>
    </MSection>
  </CfgSub>

  <template v-if="advancedOpen">
    <CfgSub v-for="group in advancedGroups" :id="DS5_SUB.group(group.id)" :key="group.id" :title="t(group.titleKey)">
      <DualSenseLive :id="'g' + group.id" />
      <MSection v-for="(rows, index) in wiredRows(group.wired)" :key="'w' + index">
        <MGroup>
          <template v-for="row in rows" :key="row.key.id">
            <RangeField v-if="row.pair" :low="row.key" :high="row.high" :low-value="pair(row.key)" :high-value="pair(row.high)" @update:low="setValue(row.key.id, $event)" @update:high="setValue(row.high.id, $event)" @reset="resetKey(row.key.id); resetKey(row.high.id)" />
            <ControlField v-else :descriptor="row.key" :model-value="edited[row.key.id]" @update:model-value="onValue(row.key.id, $event)" @reset="resetKey(row.key.id)" />
          </template>
        </MGroup>
      </MSection>
    </CfgSub>

    <!-- The keys this host build does not apply: the client's own, shown as the client's; and the ones not available yet -->
    <CfgSub :id="DS5_SUB.pending" :title="t('ds5.pending.title')">
      <MSection v-if="unavailable.client.length" :title="t('ds5.client.title')">
        <MGroup>
          <MItem v-for="key in unavailable.client" :key="key.id" :title="localizedKeyLabel(key, locale)" :text="valueText(key)" icon="devices" :data-ds5-key="key.id" :data-cfg="`ds5:${key.key}`" :data-tip="key.reason">
            <template #trail><MChip tone="info">{{ t('ds5.client.chip') }}</MChip></template>
          </MItem>
        </MGroup>
        <MFooter>{{ t('ds5.client.hint') }}</MFooter>
      </MSection>
      <MSection v-if="unavailable.pending.length" :title="t('ds5.pending.not_yet')">
        <MGroup>
          <ControlField v-for="key in unavailable.pending" :key="key.id" :descriptor="key" :model-value="edited[key.id]" disabled />
        </MGroup>
        </MSection>
    </CfgSub>
  </template>

  <CfgSub id="ds5-test" :title="t('ds5.test_panel.title')">
    <MSection>
      <MGroup>
        <MItem icon="gamepad" :title="t('ds5.test_panel.title')" :text="testNote || t('ds5.test_panel.note')" stack>
          <template #control>
            <div class="ds5-test">
              <MButton v-for="action in TESTS" :key="action" variant="tonal" size="sm" @click="runTest(action)">{{ t('ds5.test_panel.' + action) }}</MButton>
            </div>
          </template>
        </MItem>
        <MItem v-if="embedded" action icon="open_in_new" :title="t('ds5.test.open_tool')" @click="openTool" />
      </MGroup>
    </MSection>
  </CfgSub>

  <AdvancedWarningDialog :show="warningOpen" @cancel="cancelWarning" @confirm="confirmWarning" />
  <MSnackbar v-model="snack" :message="saveMessage" />
</template>

<style>
.ds5-test { display: flex; flex-wrap: wrap; gap: 8px; }
.ds5-hit { animation: ds5-hit 2.2s ease-out; }
@keyframes ds5-hit { 0%, 40% { box-shadow: inset 0 0 0 3px var(--md-sys-color-primary); } 100% { box-shadow: inset 0 0 0 3px transparent; } }
</style>
