<script setup>
import { ref, computed } from 'vue'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../components/layout/PlatformLayout.vue'

const props = defineProps([
  'platform',
  'config'
])

const config = ref(props.config)

// 按 name 去重，同一名称只保留一项（保持首次出现顺序）
const uniqueAdapters = computed(() => {
  const list = config.value?.adapters ?? []
  const seen = new Set()
  return list.filter((a) => {
    const name = a?.name ?? ''
    if (seen.has(name)) return false
    seen.add(name)
    return true
  })
})
</script>

<template>
  <div class="mb-3" v-if="platform !== 'macos'">
    <label for="adapter_name" class="form-label">{{ $t('config.adapter_name') }}</label>
    <PlatformLayout :platform="platform">
      <template #windows>
        <select id="adapter_name" class="form-select" v-model="config.adapter_name">
          <option value="">{{ $t("_common.autodetect") }}</option>
          <option v-for="(adapter, index) in uniqueAdapters" :value="adapter.name" :key="index">
            {{ adapter.name }}
          </option>
        </select>
      </template>
      <template #linux>
        <input type="text" class="form-control" id="adapter_name"
           :placeholder="$tp('config.adapter_name_placeholder', '/dev/dri/renderD128')"
           v-model="config.adapter_name" />
      </template>
    </PlatformLayout>
    <div class="form-text">
      <PlatformLayout :platform="platform">
        <template #windows>
          {{ $t('config.adapter_name_desc_windows') }}<br>
        </template>
        <template #linux>
          {{ $t('config.adapter_name_desc_linux_1') }}<br>
          <pre>ls /dev/dri/renderD*  # {{ $t('config.adapter_name_desc_linux_2') }}</pre>
          <pre>
              vainfo --display drm --device /dev/dri/renderD129 | \
                grep -E "((VAProfileH264High|VAProfileHEVCMain|VAProfileHEVCMain10).*VAEntrypointEncSlice)|Driver version"
            </pre>
          {{ $t('config.adapter_name_desc_linux_3') }}<br>
          <i>VAProfileH264High   : VAEntrypointEncSlice</i>
        </template>
      </PlatformLayout>
    </div>
  </div>
</template>
