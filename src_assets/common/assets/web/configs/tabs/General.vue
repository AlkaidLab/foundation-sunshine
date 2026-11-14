<script setup>
import { reactive, ref } from 'vue'
import { computed } from 'vue'
import { MFooter, MGroup, MInfo, MSection, guideTexts } from 'foundation-ui/vue'
import { useI18n } from 'vue-i18n'
import { detectSystemLocale, localeChoice, resolveChoice, setLocaleChoice } from '../../config/i18n.js'
import CommandTable from '../../components/CommandTable.vue'
import CfgChoice from '../../components/cfg/CfgChoice.vue'
import CfgExpert from '../../components/cfg/CfgExpert.vue'
import { isChanged } from 'foundation-ui/settings/index.js'
import CfgField from '../../components/cfg/CfgField.vue'
import CfgRow from '../../components/cfg/CfgRow.vue'
import CfgSub from '../../components/cfg/CfgSub.vue'
import CfgSwitch from '../../components/cfg/CfgSwitch.vue'
import CfgTop from '../../components/cfg/CfgTop.vue'

const props = defineProps({
  platform: String,
  config: Object,
  globalPrepCmd: Array,
})

const { t, locale } = useI18n()
const config = ref(props.config)
const globalPrepCmd = ref(props.globalPrepCmd)
const isEmbeddedGui = window.isTauri === true && window.parent !== window

function addCmd() {
  let template = {
    do: '',
    undo: '',
  }

  if (props.platform === 'windows') {
    template = { ...template, elevated: false }
  }
  globalPrepCmd.value.push(template)
}

function removeCmd(index) {
  globalPrepCmd.value.splice(index, 1)
}

function handleCommandOrderChanged(newOrder) {
  // 更新命令顺序
  globalPrepCmd.value.splice(0, globalPrepCmd.value.length, ...newOrder)
}

const LOCALES = [
  ['en', 'English'], ['en_GB', 'English (UK)'], ['en_US', 'English (US)'], ['zh', '简体中文'], ['zh_TW', '繁體中文'],
  ['de', 'Deutsch'], ['fr', 'Français'], ['es', 'Español'], ['it', 'Italiano'], ['ja', '日本語'], ['ko', '한국어'],
  ['ru', 'Русский'], ['uk', 'Українська'], ['pt', 'Português'], ['pt_BR', 'Português (Brasil)'], ['pl', 'Polski'],
  ['sv', 'Svenska'], ['tr', 'Türkçe'], ['cs', 'Čeština'], ['bg', 'Български'],
].map(([value, label]) => ({ value, label }))
// 跟随系统 (the default) is a choice of this window, stored as `system`; the host's own language (config locale) keeps the code that is shown
const localeOptions = computed(() => [{ value: 'system', label: `${guideTexts(String(locale.value)).intro.followSystem} · ${LOCALES.find((o) => o.value === detectSystemLocale())?.label || detectSystemLocale()}` }, ...LOCALES])
const localeProxy = reactive({
  get locale() { return localeChoice.value },
  set locale(choice) { void setLocaleChoice(choice); config.value.locale = resolveChoice(choice) },
})
// The host writes a level by name ("debug") and reads either the name or its number, so the names are the values and the
// numbers are accepted for a configuration that kept one.
const LOG_LEVELS = ['verbose', 'debug', 'info', 'warning', 'error', 'fatal', 'none']
const logLevels = computed(() => LOG_LEVELS.map((name, n) => ({ value: name, aliases: [String(n)], label: t(`config.log_level_${n}`) })))
const sleepModes = computed(() => [['0', 'suspend'], ['1', 'hibernate'], ['2', 'away']].map(([value, key]) => ({ value, label: t(`config.sleep_mode_${key}`) })))
</script>

<template>
  <CfgTop>
    <MSection>
      <MGroup>
        <CfgChoice :config="localeProxy" k="locale" :options="localeOptions" />
        <CfgField :config="config" k="sunshine_name" placeholder="Sunshine" />
        <CfgChoice :config="config" k="sleep_mode" :options="sleepModes" :icons="{ 0: 'bedtime', 1: 'power_settings_new', 2: 'visibility' }" />
      </MGroup>
    </MSection>

    <MSection>
      <MGroup>
        <CfgSwitch :config="config" k="notify_pre_releases" :on="true" :off="false" def="false" />
        <CfgSwitch v-if="!isEmbeddedGui" :config="config" k="system_tray" :on="true" :off="false" def="true" />
        <CfgSwitch :config="config" k="stop_on_last_video_session" :on="true" :off="false" def="false" />
        <CfgSwitch :config="config" k="launch_splash" :on="true" :off="false" def="true" />
      </MGroup>
    </MSection>
  </CfgTop>

  <CfgSub id="expert" :title="$t('tabs.advanced')" inline>
    <MSection :title="$t('tabs.advanced')">
      <MGroup>
        <CfgExpert :config="config" k="min_log_level" control="sheet" :options="logLevels" label="config.log_level" help="config.log_level_desc" def="2" />
      </MGroup>
    </MSection>
  </CfgSub>

  <CfgSub id="prep" :title="$t('config.global_prep_cmd')" inline>
    <MSection :title="$t('config.global_prep_cmd')">
      <template #action><MInfo :text="$t('config.global_prep_cmd_desc')" /></template>
      <CommandTable :commands="globalPrepCmd" :platform="platform" type="prep" @add-command="addCmd" @remove-command="removeCmd" @order-changed="handleCommandOrderChanged" />
    </MSection>
    <MFooter>{{ $t('config.global_prep_cmd_desc') }}</MFooter>
  </CfgSub>
</template>
