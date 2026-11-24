<template>
  <MItem class="spx-pick" wrap :action="action" :selected="selected" :title="title || C.games[install.game] || install.game" @click="$emit('click')">
    <template #lead><img class="spx-icon" :src="icon" alt="" draggable="false" @error="hide"></template>
    <template #default><span class="spx-mono" :title="full">{{ midCut(full, 96) }}</span></template>
    <template #trail>
      <MChip v-if="regionLabel">{{ regionLabel }}</MChip>
      <slot name="trail" />
      <MIcon v-if="selected" name="check" class="spx-check" />
    </template>
  </MItem>
</template>

<script setup>
import { computed } from 'vue'
import { MChip, MIcon, MItem } from 'foundation-ui/vue'
import { useSparxieCopy } from '../../composables/useSparxieCopy.js'
import { artUrl, fullPath, gameArtId, midCut } from '../../utils/sparxie/model.js'

// One install as a row: the game's icon, its name, the region and the path of the executable. The row of 「新建 Sparxie 应用」 (the install the host found or the person chose), also used wherever an install is listed.
const props = defineProps({
  install: { type: Object, required: true },
  /** the title's name (the host's list names the titles; a title this build has no name for shows its id) */
  title: { type: String, default: '' },
  /** the region that was chosen, for an install whose own region is still open */
  region: { type: String, default: '' },
  /** the version of the official poster art (the address changes with it) */
  version: { type: String, default: '' },
  selected: { type: Boolean, default: false },
  action: { type: Boolean, default: false },
})
defineEmits(['click'])
const C = useSparxieCopy()
const full = computed(() => fullPath(props.install))
const regionId = computed(() => props.install.region || props.region)
const regionLabel = computed(() => (regionId.value ? C.value.regions[regionId.value] : ''))
const icon = computed(() => artUrl('icon', { id: gameArtId(props.install.game, regionId.value || 'cn') }, props.version))
const hide = (event) => { event.target.style.visibility = 'hidden' }
</script>

<style scoped>
.spx-icon { width: 40px; height: 40px; border-radius: 10px; object-fit: cover; display: block; }
.spx-mono { display: block; min-width: 0; font-family: var(--m-font-mono, ui-monospace, monospace); font-size: 12.5px; line-height: 1.35; overflow-wrap: anywhere; color: var(--md-sys-color-on-surface-variant); }
.spx-pick :deep(.m-item__lead:has(.spx-icon)) { width: 40px; height: 40px; }
.spx-check { color: var(--md-sys-color-primary); }
</style>
