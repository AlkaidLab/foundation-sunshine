<template>
  <div class="m-app m-root stream-timer-page">
    <MPage :title="T.title">
      <div class="m-stream-timer" :style="{ '--stream-timer-scale': scale }" role="timer" aria-live="off">
        <div class="m-stream-timer__face">
          <MIcon name="schedule" size="32" />
          <strong>{{ time }}</strong>
          <span>{{ date }}</span>
        </div>
      </div>
      <MGroup>
        <MItem icon="schedule" :title="T.scale" :value="`${Math.round(scale * 100)}%`" />
        <MItem stack :title="T.adjust">
          <template #control>
            <div class="m-stream-timer__controls">
              <MButton variant="tonal" icon="remove" :aria-label="T.decrease" :disabled="scale <= MIN_SCALE" @click="scale = Math.max(MIN_SCALE, scale - STEP)" />
              <MButton variant="tonal" icon="add" :aria-label="T.increase" :disabled="scale >= MAX_SCALE" @click="scale = Math.min(MAX_SCALE, scale + STEP)" />
              <MButton variant="text" icon="restart_alt" :aria-label="T.reset" @click="scale = 1" />
            </div>
          </template>
        </MItem>
      </MGroup>
    </MPage>
  </div>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { MButton, MGroup, MIcon, MItem, MPage } from 'foundation-ui/vue'
import { streamTimerTexts } from './streamTimerTexts.js'

// 串流计时器 (system): the clock to read while streaming, at a size to choose. The same page as the control panel's (docs/parity.md): the time, the date, the size.
const MIN_SCALE = 0.6
const MAX_SCALE = 1.4
const STEP = 0.2
const { locale } = useI18n()
const T = computed(() => streamTimerTexts(String(locale.value)))
const tag = computed(() => (/^zh/i.test(String(locale.value)) ? 'zh-CN' : String(locale.value).replace('_', '-')))
const now = ref(new Date())
const scale = ref(1)
let ticker = 0
function formatter(options) {
  return computed(() => {
    try { return new Intl.DateTimeFormat(tag.value, options) }
    catch { return new Intl.DateTimeFormat('en', options) }
  })
}
const clockFormat = formatter({ hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false })
const dateFormat = formatter({ weekday: 'long', year: 'numeric', month: 'long', day: 'numeric' })
const time = computed(() => clockFormat.value.format(now.value))
const date = computed(() => dateFormat.value.format(now.value))
function tick() {
  now.value = new Date()
  ticker = window.setTimeout(tick, 1000 - now.value.getMilliseconds())
}
onMounted(tick)
onBeforeUnmount(() => window.clearTimeout(ticker))
</script>

<style scoped>
.m-stream-timer { display: grid; place-items: center; min-height: min(46vh, 420px); padding: 24px; }
.m-stream-timer__face { display: grid; justify-items: center; gap: 10px; transform: scale(var(--stream-timer-scale)); transform-origin: center; color: var(--md-sys-color-on-surface); }
.m-stream-timer__face strong { font: var(--m-display-medium); font-variant-numeric: tabular-nums; letter-spacing: 0; }
.m-stream-timer__face span { font: var(--m-title-medium); color: var(--md-sys-color-on-surface-variant); }
.m-stream-timer__controls { display: flex; gap: 8px; }
</style>
