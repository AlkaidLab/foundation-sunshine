<template>
  <span class="cover-badge" :class="`cover-badge--${kind}`" aria-hidden="true"><img :src="SPARXIE_MARK" alt="" draggable="false" /></span>
</template>

<script setup>
import { SPARXIE_MARK } from '../../utils/sparxie/model.js'

// The Sparxie mark as the host draws it on a poster (a plate of smoked glass in the top-right corner: its side is the smaller of 26% of the width and 25% of the height,
// its margin 14% of its side). This is for a cover of the person's own, whose badged copy the host makes when the application is saved; an official poster is shown from the file the host made.
defineProps({ kind: { type: String, default: 'poster' } }) // poster (2:3) | hero (16:9)
</script>

<style scoped>
.cover-badge {
  position: absolute;
  display: block;
  aspect-ratio: 1;
  border-radius: 30%;
  background: color-mix(in srgb, var(--md-sys-color-scrim) 42%, transparent);
  box-shadow: inset 0 0 0 1px color-mix(in srgb, var(--md-sys-color-inverse-on-surface) 32%, transparent), 0 2px 6px color-mix(in srgb, var(--md-sys-color-scrim) 40%, transparent);
  backdrop-filter: blur(8px);
}

/* the frame is 2:3: 26% of the width, a margin of 14% of that */
.cover-badge--poster {
  top: 2.43%;
  right: 3.64%;
  width: 26%;
}

/* the frame is 16:9: the height limits it, 25% of it is 14.06% of the width */
.cover-badge--hero {
  top: 3.5%;
  right: 1.97%;
  width: 14.06%;
}

.cover-badge img {
  position: absolute;
  inset: 14%;
  display: block;
  width: 72%;
  height: 72%;
  object-fit: contain;
}
</style>
