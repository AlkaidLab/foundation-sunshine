<script setup>
import { computed } from 'vue'
import { useCfgText } from '../../composables/useCfgText.js'
import { MValueItem } from 'foundation-ui/vue'
import { browsePath } from '../../utils/browsePath.js'

// One configuration setting that is a file or a folder: a row with the path; the dialog it opens has the field and, where the product can open a chooser, a browse button.
const props = defineProps({
  config: { type: Object, required: true },
  k: { type: String, required: true },
  label: { type: String, default: '' },
  help: { type: String, default: '' },
  placeholder: { type: String, default: '' },
  tp: { type: Boolean, default: false },
  /** 'file' | 'folder' */
  kind: { type: String, default: 'file' },
  disabled: { type: Boolean, default: false },
})
const { t, title, info } = useCfgText(props)
const model = computed({
  get: () => props.config[props.k] ?? '',
  set: (value) => { props.config[props.k] = value },
})
const browse = browsePath(props.kind, () => title.value)
</script>

<template>
  <MValueItem v-model="model" :data-cfg="k" :label="title" :placeholder="placeholder" :help="info" :browse="browse" :browse-label="t('_common.browse')" :browse-icon="kind === 'folder' ? 'folder_open' : 'upload_file'" :disabled="disabled" :ok-label="t('_common.ok')" :cancel-label="t('_common.cancel')" :empty-text="t('_common.auto')" />
</template>
