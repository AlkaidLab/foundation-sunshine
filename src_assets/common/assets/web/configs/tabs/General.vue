<script setup>
import { ref } from 'vue'
import Checkbox from '../../components/Checkbox.vue'
import CommandTable from '../../components/CommandTable.vue'

const props = defineProps({
  platform: String,
  config: Object,
  globalPrepCmd: Array,
})

const config = ref(props.config)
const globalPrepCmd = ref(props.globalPrepCmd)

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
</script>

<template>
  <div id="general" class="config-page">
    <!-- Locale -->
    <div class="mb-3">
      <label for="locale" class="form-label">{{ $t('config.locale') }}</label>
      <select id="locale" class="form-select" v-model="config.locale">
        <option value="bg">Български (Bulgarian)</option>
        <option value="cs">Čeština (Czech)</option>
        <option value="de">Deutsch (German)</option>
        <option value="en">English</option>
        <option value="en_GB">English, UK</option>
        <option value="en_US">English, US</option>
        <option value="es">Español (Spanish)</option>
        <option value="fr">Français (French)</option>
        <option value="it">Italiano (Italian)</option>
        <option value="ja">日本語 (Japanese)</option>
        <option value="pt">Português (Portuguese)</option>
        <option value="ru">Русский (Russian)</option>
        <option value="sv">svenska (Swedish)</option>
        <option value="tr">Türkçe (Turkish)</option>
        <option value="zh">简体中文 (Chinese Simplified)</option>
        <option value="zh_TW">繁體中文 (Chinese Traditional)</option>
      </select>
      <div class="form-text">{{ $t('config.locale_desc') }}</div>
    </div>

    <!-- Sunshine Name -->
    <div class="mb-3">
      <label for="sunshine_name" class="form-label">{{ $t('config.sunshine_name') }}</label>
      <input
        type="text"
        class="form-control"
        id="sunshine_name"
        placeholder="Sunshine"
        v-model="config.sunshine_name"
      />
      <div class="form-text">{{ $t('config.sunshine_name_desc') }}</div>
    </div>

    <!-- Log Level -->
    <div class="mb-3">
      <label for="min_log_level" class="form-label">{{ $t('config.log_level') }}</label>
      <select id="min_log_level" class="form-select" v-model="config.min_log_level">
        <option value="0">{{ $t('config.log_level_0') }}</option>
        <option value="1">{{ $t('config.log_level_1') }}</option>
        <option value="2">{{ $t('config.log_level_2') }}</option>
        <option value="3">{{ $t('config.log_level_3') }}</option>
        <option value="4">{{ $t('config.log_level_4') }}</option>
        <option value="5">{{ $t('config.log_level_5') }}</option>
        <option value="6">{{ $t('config.log_level_6') }}</option>
      </select>
      <div class="form-text">{{ $t('config.log_level_desc') }}</div>
    </div>

    <!-- Global Prep Commands -->
    <div class="mb-3">
      <label class="form-label">{{ $t('config.global_prep_cmd') }}</label>
      <div class="form-text">{{ $t('config.global_prep_cmd_desc') }}</div>
      <CommandTable
        class="mt-3"
        :commands="globalPrepCmd"
        :platform="platform"
        type="prep"
        @add-command="addCmd"
        @remove-command="removeCmd"
        @order-changed="handleCommandOrderChanged"
      />
    </div>

    <!-- Notify Pre-Releases -->
    <Checkbox
      class="mb-3"
      id="notify_pre_releases"
      locale-prefix="config"
      v-model="config.notify_pre_releases"
      default="false"
    ></Checkbox>

    <!-- Enable system tray -->
    <Checkbox
      class="mb-3"
      id="system_tray"
      locale-prefix="config"
      v-model="config.system_tray"
      default="true"
    ></Checkbox>
  </div>
</template>

<style scoped></style>
