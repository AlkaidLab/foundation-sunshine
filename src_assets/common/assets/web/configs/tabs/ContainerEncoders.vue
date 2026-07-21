<script setup>
import { defineAsyncComponent, ref } from 'vue'

const NvidiaNvencEncoder = defineAsyncComponent(() => import('./encoders/NvidiaNvencEncoder.vue'))
const IntelQuickSyncEncoder = defineAsyncComponent(() => import('./encoders/IntelQuickSyncEncoder.vue'))
const AmdAmfEncoder = defineAsyncComponent(() => import('./encoders/AmdAmfEncoder.vue'))
const VideotoolboxEncoder = defineAsyncComponent(() => import('./encoders/VideotoolboxEncoder.vue'))
const SoftwareEncoder = defineAsyncComponent(() => import('./encoders/SoftwareEncoder.vue'))

const props = defineProps([
  'platform',
  'config',
  'currentTab'
])

const config = ref(props.config)
</script>

<template>

  <!-- NVIDIA NVENC Encoder Tab -->
  <NvidiaNvencEncoder
      v-if="currentTab === 'nv'"
      :platform="platform"
      :config="config"
  />

  <!-- Intel QuickSync Encoder Tab -->
  <IntelQuickSyncEncoder
      v-if="currentTab === 'qsv'"
      :platform="platform"
      :config="config"
  />

  <!-- AMD AMF Encoder Tab -->
  <AmdAmfEncoder
      v-if="currentTab === 'amd'"
      :platform="platform"
      :config="config"
  />

  <!-- VideoToolbox Encoder Tab -->
  <VideotoolboxEncoder
      v-if="currentTab === 'vt'"
      :platform="platform"
      :config="config"
  />

  <!-- Software Encoder Tab -->
  <SoftwareEncoder
      v-if="currentTab === 'sw'"
      :platform="platform"
      :config="config"
  />

</template>
