<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { $tp } from '../../../platform-i18n'
import CfgChoice from '../../../components/cfg/CfgChoice.vue'
import CfgField from '../../../components/cfg/CfgField.vue'

const props = defineProps(['platform', 'config'])
const { t } = useI18n()

// 按 name 去重，同一名称只保留一项（保持首次出现顺序）
const uniqueAdapters = computed(() => {
  const list = props.config?.adapters ?? []
  const seen = new Set()
  return list.filter((a) => {
    const name = a?.name ?? ''
    if (seen.has(name)) return false
    seen.add(name)
    return true
  })
})
const options = computed(() => [{ value: '', label: t('_common.autodetect') }, ...uniqueAdapters.value.map((a) => ({ value: a.name, label: a.name }))])
</script>

<template>
  <template v-if="platform !== 'macos'">
    <CfgChoice v-if="platform === 'windows'" :config="config" k="adapter_name" :options="options" def="" help="none" />
    <CfgField v-else :config="config" k="adapter_name" :placeholder="$tp('config.adapter_name_placeholder', '/dev/dri/renderD128')" help="none" />
  </template>
</template>
