<template>
  <MSection v-if="state.loaded && state.available" :title="C.configTitle" class="spx-shared">
    <MGroup>
      <MItem wrap icon="description" :title="state.sharedConfigDir || C.configBuiltin">
        <template #trail>
          <MButton v-if="state.sharedConfigDir" variant="text" :disabled="busy" @click="select('')">{{ C.configReset }}</MButton>
          <MButton variant="text" :disabled="busy" @click="browsing = !browsing">{{ C.configChoose }}</MButton>
        </template>
      </MItem>
    </MGroup>
    <MBanner v-if="error" tone="error" icon="error">{{ error }}</MBanner>
    <SparxieBrowse v-if="browsing" kind="folder" :title="C.browseTitleSparxie" :start="state.sharedConfigDir" @choose="select" @cancel="browsing = false" />
  </MSection>
</template>

<script setup>
import { onMounted, ref } from 'vue'
import { MBanner, MButton, MGroup, MItem, MSection } from 'foundation-ui/vue'
import SparxieBrowse from './SparxieBrowse.vue'
import { useSparxie } from '../../composables/useSparxie.js'
import { useSparxieCopy } from '../../composables/useSparxieCopy.js'
import { failureText } from '../../utils/sparxie/copy.js'

// Whose config.json the host uses: the component's own, or that of a Sparxie the person keeps elsewhere. The host runs only its component.
const emit = defineEmits(['changed'])
const C = useSparxieCopy()
const { state, client, refresh } = useSparxie()
const browsing = ref(false)
const busy = ref(false)
const error = ref('')

async function select(path) {
  busy.value = true; error.value = ''; browsing.value = false
  try {
    await client().selectSharedConfig(path)
    await refresh()
    emit('changed')
  } catch (e) {
    error.value = failureText(C.value, C.value.saveFailed, e)
  } finally { busy.value = false }
}
onMounted(() => refresh())
</script>

<style scoped>
.spx-shared { margin-top: 16px; }
</style>
