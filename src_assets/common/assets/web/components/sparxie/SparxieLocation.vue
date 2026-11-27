<template>
  <MSection v-if="state.loaded && state.available" :title="C.locationTitle" class="spx-location">
    <MBanner v-if="state.external && !state.installed" tone="error" icon="error">{{ C.errors.sparxie_external_missing }}</MBanner>
    <MGroup>
      <MItem wrap icon="folder" :title="state.external ? state.externalDir : C.locationBuiltin">
        <template #trail>
          <MButton v-if="state.external" variant="text" :disabled="busy" @click="select('')">{{ C.locationReset }}</MButton>
          <MButton variant="text" :disabled="busy" @click="browsing = !browsing">{{ C.locationChoose }}</MButton>
        </template>
      </MItem>
    </MGroup>
    <MBanner v-if="error" tone="error" icon="error">{{ error }}</MBanner>
    <SparxieBrowse v-if="browsing" kind="folder" :title="C.browseTitleSparxie" :start="state.externalDir" @choose="select" @cancel="browsing = false" />
  </MSection>
</template>

<script setup>
import { onMounted, ref } from 'vue'
import { MBanner, MButton, MGroup, MItem, MSection } from 'foundation-ui/vue'
import SparxieBrowse from './SparxieBrowse.vue'
import { useSparxie } from '../../composables/useSparxie.js'
import { useSparxieCopy } from '../../composables/useSparxieCopy.js'
import { failureText } from '../../utils/sparxie/copy.js'

// Which Sparxie the host uses: its own component, or an install the person keeps elsewhere.
const emit = defineEmits(['changed'])
const C = useSparxieCopy()
const { state, client, refresh } = useSparxie()
const browsing = ref(false)
const busy = ref(false)
const error = ref('')

async function select(path) {
  busy.value = true; error.value = ''; browsing.value = false
  try {
    await client().selectLocation(path)
    await refresh()
    emit('changed')
  } catch (e) {
    error.value = failureText(C.value, C.value.saveFailed, e)
  } finally { busy.value = false }
}
onMounted(() => refresh())
</script>

<style scoped>
.spx-location { margin-top: 16px; }
</style>
