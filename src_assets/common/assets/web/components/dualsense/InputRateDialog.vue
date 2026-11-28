<script setup>
import { computed, onBeforeUnmount, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { MDialog, MButton } from 'foundation-ui/vue'
import { inputRateWarning, INPUT_RATE_WARNING_SECONDS } from 'foundation-ui/settings/index.js'
import { remainingSeconds } from '../../utils/advancedWarning.js'

// The confirm before the input report rate of a controller goes above 250 Hz. The text, the figures and the countdown come from
// foundation-ui, so the Panel says the same; the button unlocks when the countdown is over.
// option: the speed being chosen, the text quotes its measured figures
const props = defineProps({ show: { type: Boolean, default: false }, option: { type: String, default: 'auto' } })
const emit = defineEmits(['confirm', 'cancel'])

const { locale } = useI18n()
const text = computed(() => inputRateWarning(locale.value, props.option))
const seconds = ref(INPUT_RATE_WARNING_SECONDS)
const ready = computed(() => seconds.value === 0)
let startedAt = 0
let timer = null

const stop = () => {
  if (timer !== null) clearInterval(timer)
  timer = null
}
const tick = () => {
  seconds.value = remainingSeconds(Date.now() - startedAt, INPUT_RATE_WARNING_SECONDS)
  if (seconds.value === 0) stop()
}
watch(() => props.show, (show) => {
  stop()
  if (!show) return
  startedAt = Date.now()
  seconds.value = INPUT_RATE_WARNING_SECONDS
  timer = setInterval(tick, 250)
}, { immediate: true })
onBeforeUnmount(stop)
</script>

<template>
  <MDialog :model-value="show" :title="text.title" icon="warning" @update:model-value="(open) => { if (!open) emit('cancel') }" @cancel="emit('cancel')">
    <p v-for="line in text.lines" :key="line">{{ line }}</p>
    <template #actions>
      <MButton variant="text" @click="emit('cancel')">{{ text.cancel }}</MButton>
      <MButton variant="warning" :disabled="!ready" @click="ready && emit('confirm')">{{ ready ? text.confirm : text.wait.replace('{seconds}', String(seconds)) }}</MButton>
    </template>
  </MDialog>
</template>
