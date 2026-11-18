<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { MFooter, MGroup, MItem, MSection, displayTreeTexts } from 'foundation-ui/vue'

// 画质增强 is a page of the control panel (the DLSS noise reduction and the components behind it). In a browser this place only points there.
defineProps({ platform: { type: String, default: 'windows' } })
const { locale } = useI18n()
const x = computed(() => displayTreeTexts(locale.value))
const inFrame = typeof window !== 'undefined' && window.parent !== window
const open = () => { if (inFrame) window.parent.postMessage({ type: 'foundation-open', page: 'hdr-enhanced' }, '*') }
</script>

<template>
  <MSection :title="x.sections.quality.nr">
    <MGroup>
      <MItem action icon="wand_stars" :title="x.quality.nr" :text="inFrame ? '' : x.hdr.calibrateHost" :disabled="!inFrame" @click="open" />
    </MGroup>
    <MFooter v-if="!inFrame">{{ x.common.hostOnly }}</MFooter>
  </MSection>
</template>
