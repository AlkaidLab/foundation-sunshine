<template>
  <div ref="strip" class="cover-strip" :class="[`cover-strip--${orientation}`, { 'fade-start': !atStart, 'fade-end': !atEnd }]" role="radiogroup" :aria-label="ariaLabel" @scroll.passive="measure">
    <button
      v-for="option in options"
      :key="option.value"
      type="button"
      role="radio"
      class="cover-strip__item"
      :class="{ 'is-on': option.value === modelValue }"
      :aria-checked="option.value === modelValue"
      :data-style="option.value"
      :disabled="disabled"
      @click="emit('update:modelValue', option.value)"
    >
      <span class="cover-strip__thumb">
        <img v-if="option.src && !failed.has(option.src)" :src="option.src" alt="" loading="lazy" decoding="async" @error="failed.add(option.src)">
        <MIcon v-else name="palette" class="cover-strip__ph" />
        <MIcon v-if="option.value === modelValue" name="check" class="cover-strip__check" />
      </span>
      <span class="cover-strip__label">{{ option.label }}</span>
    </button>
  </div>
</template>

<script setup>
// the six generated cover styles (and 跟随全局) as a strip of small live thumbnails: the host's own rendering of this application, portrait or landscape
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { MIcon } from 'foundation-ui/vue'
import { COVER_STYLES, COVER_STYLE_AUTO, coverPreviewUrl, normalizeCoverStyle } from '../utils/generatedCover.js'

const props = defineProps({
  /** one of the six style names, or 'auto' (follow the global style) */
  modelValue: { type: String, default: COVER_STYLE_AUTO },
  /** the host's id of an application: the thumbnails are its covers */
  appId: { type: String, default: '' },
  /** the saved name of the application (the host renders from it) */
  name: { type: String, default: '' },
  orientation: { type: String, default: 'portrait' },
  /** offer 跟随全局 as the first choice (an application), not for the global setting itself */
  allowAuto: { type: Boolean, default: false },
  /** the global style, drawn on the 跟随全局 thumbnail */
  globalStyle: { type: String, default: '' },
  disabled: { type: Boolean, default: false },
  ariaLabel: { type: String, default: '' },
})
const emit = defineEmits(['update:modelValue'])
const { t } = useI18n()

// a thumbnail the host could not draw shows a palette glyph instead
const failed = reactive(new Set())
watch(() => [props.appId, props.name], () => failed.clear())

const srcOf = (style) => coverPreviewUrl(props.appId, style, props.orientation, props.name)
const options = computed(() => {
  const list = COVER_STYLES.map((style) => ({ value: style, label: t(`apps.cover_style_${style}`), src: srcOf(style) }))
  if (!props.allowAuto) return list
  return [{ value: COVER_STYLE_AUTO, label: t('apps.cover_style_auto'), src: srcOf(normalizeCoverStyle(props.globalStyle)) }, ...list]
})

// the chosen one is in view when the strip is longer than its card
const strip = ref(null)
const reveal = () => nextTick(() => strip.value?.querySelector('.is-on')?.scrollIntoView?.({ block: 'nearest', inline: 'nearest' }))
// a strip longer than its card fades at the edge that has more behind it
const atStart = ref(true)
const atEnd = ref(true)
const measure = () => {
  const el = strip.value
  if (!el) return
  atStart.value = el.scrollLeft <= 2
  atEnd.value = el.scrollLeft + el.clientWidth >= el.scrollWidth - 2
}
let observer = null
onMounted(() => {
  reveal()
  measure()
  if (typeof ResizeObserver !== 'undefined' && strip.value) { observer = new ResizeObserver(measure); observer.observe(strip.value) }
})
onBeforeUnmount(() => observer?.disconnect())
watch(() => props.orientation, () => { reveal(); nextTick(measure) })
</script>

<style scoped>
.cover-strip {
  display: flex;
  gap: 6px;
  min-width: 0;
  padding: 2px;
  overflow-x: auto;
  overscroll-behavior-x: contain;
  scroll-snap-type: x proximity;
  scrollbar-width: none;
}
.cover-strip::-webkit-scrollbar { display: none; }
.cover-strip.fade-end { mask-image: linear-gradient(to right, #000 calc(100% - 28px), transparent); }
.cover-strip.fade-start { mask-image: linear-gradient(to left, #000 calc(100% - 28px), transparent); }
.cover-strip.fade-start.fade-end { mask-image: linear-gradient(to right, transparent, #000 28px, #000 calc(100% - 28px), transparent); }

.cover-strip__item {
  flex: none;
  display: grid;
  justify-items: center;
  gap: 6px;
  padding: 6px 6px 8px;
  border: 0;
  border-radius: 14px;
  background: transparent;
  color: var(--md-sys-color-on-surface-variant);
  font: inherit;
  cursor: pointer;
  scroll-snap-align: start;
  -webkit-tap-highlight-color: transparent;
  transition: background-color 150ms, color 150ms;
}
.cover-strip__item:hover:not(:disabled) { background: var(--md-sys-color-surface-container-high); }
.cover-strip__item:focus-visible { outline: 2px solid var(--md-sys-color-primary); outline-offset: 1px; }
.cover-strip__item:disabled { opacity: 0.5; cursor: default; }
.cover-strip__item.is-on { background: var(--md-sys-color-secondary-container); color: var(--md-sys-color-on-secondary-container); }

.cover-strip__thumb {
  position: relative;
  display: grid;
  place-items: center;
  overflow: hidden;
  border-radius: 8px;
  background: var(--md-sys-color-surface-container-highest);
}
.cover-strip--portrait .cover-strip__thumb { width: 48px; height: 72px; }
.cover-strip--landscape .cover-strip__thumb { width: 88px; height: 50px; }
.cover-strip__thumb img { display: block; width: 100%; height: 100%; object-fit: cover; }
.cover-strip__ph { font-size: 22px; opacity: 0.6; }
.cover-strip__check {
  position: absolute;
  right: 3px;
  bottom: 3px;
  width: 16px;
  height: 16px;
  display: grid;
  place-items: center;
  font-size: 12px;
  border-radius: 50%;
  background: var(--md-sys-color-primary);
  color: var(--md-sys-color-on-primary);
}
.cover-strip__label { max-width: 76px; font: var(--m-label-medium); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.cover-strip__item.is-on .cover-strip__label { font-weight: 600; }

@media (prefers-reduced-motion: reduce) { .cover-strip__item { transition: none; } }
</style>
