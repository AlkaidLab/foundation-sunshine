<template>
  <MGroup>
    <MSwitchItem :model-value="follow" icon="image" :title="C.coverFollow" :text="C.coverFollowText" @update:model-value="setFollow" />
    <MItem v-if="!follow" action icon="refresh" :title="C.coverRestore" @click="setFollow(true)" />
    <MSwitchItem v-if="touch" v-model="block.badge" class="spx-badge" icon="verified" :title="C.posterBadge" :text="follow ? C.posterBadgeOfficial : C.posterBadgeCustom" />
  </MGroup>
  <!-- what the clients get: the official posters the host made, with the mark drawn on them when the badge is on and plain when it is off -->
  <div v-if="follow" class="cover-pair cover-pair--preview">
    <CoverPreview kind="poster" :title="t('apps.cover_portrait')" :src="posterSrc" :text="state" />
    <CoverPreview kind="hero" :title="t('apps.cover_landscape')" :src="heroSrc" :text="state" />
  </div>
</template>

<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { MGroup, MItem, MSwitchItem } from 'foundation-ui/vue'
import { useSparxieCopy } from '../../composables/useSparxieCopy.js'
import { artUrl } from '../../utils/sparxie/model.js'
import CoverPreview from './CoverPreview.vue'

// The cover of a Sparxie application: it follows the official poster of the region (the host refreshes it with each version and only the version is stored here),
// or it is the user's own image (the picker below). A custom cover is never overwritten by the host.
// A touch application also chooses whether its poster carries the Sparxie mark: on with the official poster, off for a cover of the person's own until they turn it on
const props = defineProps({
  form: { type: Object, required: true },
  updated: { type: String, default: '' },
  touch: { type: Boolean, default: false },
  // the art id of the install (game and region) and the version of its official poster: the previews are the host's own files
  artId: { type: String, default: '' },
  artVersion: { type: String, default: '' },
})
const { t } = useI18n()
const C = useSparxieCopy()
const block = props.form.sparxie
const follow = computed(() => block.cover !== 'custom')
const badged = computed(() => props.touch && block.badge !== false)
const posterSrc = computed(() => (props.artId ? artUrl('card', { id: props.artId }, props.artVersion, badged.value) : ''))
const heroSrc = computed(() => (props.artId ? artUrl('hero', { id: props.artId }, props.artVersion, badged.value) : ''))
const state = computed(() => (props.updated ? C.value.coverUpdated(props.updated) : C.value.coverNone))
function setFollow(on) {
  block.cover = on ? 'official' : 'custom'
  block.badge = on
}
</script>
