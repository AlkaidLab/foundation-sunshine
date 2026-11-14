<script setup>
import { computed, onBeforeUnmount, ref, watch } from 'vue'
import { MDialog, MButton } from 'foundation-ui/vue'
import { ADVANCED_WARNING_SECONDS, remainingSeconds } from '../../utils/advancedWarning.js'

const props = defineProps({
  show: { type: Boolean, default: false },
})

// confirm carries the state of the "do not show again" checkbox.
const emit = defineEmits(['confirm', 'cancel'])

const seconds = ref(ADVANCED_WARNING_SECONDS)
const dontShow = ref(false)
const ready = computed(() => seconds.value === 0)
let startedAt = 0
let timer = null

const stop = () => {
  if (timer !== null) clearInterval(timer)
  timer = null
}

const tick = () => {
  seconds.value = remainingSeconds(Date.now() - startedAt)
  if (seconds.value === 0) stop()
}

watch(
  () => props.show,
  (show) => {
    stop()
    if (!show) return
    dontShow.value = false
    startedAt = Date.now()
    seconds.value = ADVANCED_WARNING_SECONDS
    timer = setInterval(tick, 250)
  },
  { immediate: true },
)

onBeforeUnmount(stop)

const confirm = () => {
  if (ready.value) emit('confirm', dontShow.value)
}
</script>

<template>
  <MDialog :model-value="show" :title="$t('ds5.advanced_warning.title')" icon="warning" @update:model-value="(open) => { if (!open) emit('cancel') }" @cancel="emit('cancel')">
    <p>{{ $t('ds5.advanced_warning.intro') }}</p>
    <ul>
      <li>{{ $t('ds5.advanced_warning.risk_input') }}</li>
      <li>{{ $t('ds5.advanced_warning.risk_disconnect') }}</li>
      <li>{{ $t('ds5.advanced_warning.risk_audio') }}</li>
      <li>{{ $t('ds5.advanced_warning.risk_later') }}</li>
    </ul>
    <p>{{ $t('ds5.advanced_warning.outro') }}</p>
    <label class="m-dialog__check"><md-checkbox touch-target="wrapper" :checked="dontShow" @change="dontShow = $event.target.checked" />{{ $t('ds5.advanced_warning.dont_show') }}</label>
    <template #actions>
      <MButton variant="text" @click="emit('cancel')">{{ $t('_common.cancel') }}</MButton>
      <MButton variant="warning" :disabled="!ready" @click="confirm">{{ ready ? $t('ds5.advanced_warning.confirm') : $t('ds5.advanced_warning.confirm_wait', { seconds }) }}</MButton>
    </template>
  </MDialog>
</template>
