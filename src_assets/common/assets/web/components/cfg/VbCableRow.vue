<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { MButton, MItem } from 'foundation-ui/vue'
import { isTauriEnv } from '../../utils/helpers.js'

// The web UI reports audio status. Installing and uninstalling belong to the
// Control Panel's component page, so there is no download link here.
const props = defineProps({
  vb: { type: Object, required: true },
  loaded: { type: Boolean, default: false },
})
const emit = defineEmits(['open-devices'])
const { t } = useI18n()
const state = computed(() => (['ready', 'partial', 'disabled', 'unavailable', 'missing'].includes(props.vb.state) ? props.vb.state : 'unknown'))
const text = computed(() => props.loaded ? t(`config.vb_cable_state_${state.value}`) : '')
const showDevices = computed(() => props.loaded && ['disabled', 'unavailable', 'partial'].includes(state.value) && isTauriEnv())
</script>

<template>
  <MItem data-vb-cable :data-vb-state="props.loaded ? state : 'loading'" icon="cable" :title="t('config.vb_cable_title')" :text="text" wrap>
    <template v-if="showDevices" #trail>
      <MButton v-if="showDevices" size="sm" variant="text" icon="open_in_new" @click="emit('open-devices')">{{ t('config.vb_cable_open_devmgmt') }}</MButton>
    </template>
  </MItem>
</template>
