<template>
  <section class="cover-card" :class="`cover-card--${kind}`" :aria-label="title">
    <header class="cover-card__head">
      <h3 class="cover-card__title">{{ title }}</h3>
      <MChip>{{ kind === 'hero' ? '16:9' : '2:3' }}</MChip>
    </header>
    <MGroup>
      <div class="m-item cover-stage">
        <div class="cover-frame" :class="{ 'is-empty': !shown }">
          <img v-if="shown" :src="src" alt="" @error="failed = true" />
          <MIcon v-else name="image" fill />
        </div>
        <span v-if="text" class="cover-stage__text">{{ text }}</span>
      </div>
    </MGroup>
  </section>
</template>

<script setup>
import { computed, ref, watch } from 'vue'
import { MChip, MGroup, MIcon } from 'foundation-ui/vue'

// A cover that is shown, not chosen: the preview frame at the cover's own aspect (2:3 portrait, 16:9 landscape) with a supporting line. A picture that cannot be loaded leaves the empty frame.
const props = defineProps({
  kind: { type: String, default: 'poster' }, // poster | hero
  title: { type: String, required: true },
  src: { type: String, default: '' },
  text: { type: String, default: '' },
})
const failed = ref(false)
watch(() => props.src, () => { failed.value = false })
const shown = computed(() => !!props.src && !failed.value)
</script>

<style scoped>
.cover-card {
  display: flex;
  flex-direction: column;
  gap: 8px;
  min-width: 0;
}

.cover-card__head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  padding: 0 16px;
}

.cover-card__title {
  margin: 0;
  font: var(--m-title-small);
  color: var(--md-sys-color-on-surface-variant);
}

.cover-stage {
  flex-direction: column;
  align-items: stretch;
  gap: 12px;
  padding: 16px;
}

.cover-frame {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 100%;
  overflow: hidden;
  color: var(--md-sys-color-on-surface-variant);
  background: var(--md-sys-color-surface-container-highest);
  border-radius: var(--md-sys-shape-corner-large);
}

.cover-card--poster .cover-frame {
  aspect-ratio: 2 / 3;
}

.cover-card--hero .cover-frame {
  aspect-ratio: 16 / 9;
}

.cover-frame img {
  display: block;
  width: 100%;
  height: 100%;
  object-fit: cover;
}

.cover-frame .m-icon {
  width: 32px;
  height: 32px;
}

.cover-stage__text {
  font: var(--m-body-medium);
  color: var(--md-sys-color-on-surface-variant);
}

/* stacked on a narrow page the portrait frame keeps a hand-sized width, centred in its card */
@media (max-width: 899.98px) {
  .cover-card--poster .cover-frame {
    width: min(100%, 220px);
    margin-inline: auto;
  }
}
</style>
