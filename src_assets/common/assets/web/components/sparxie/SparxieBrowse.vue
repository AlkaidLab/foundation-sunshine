<template>
  <MSection :title="kind === 'file' ? C.browseTitleFile : C.browseTitleFolder" class="spx-browse">
    <MGroup>
      <MItem class="spx-browse__bar" :title="here ? midCut(here, 56) : C.browseRoot" :icon="here ? 'folder_open' : 'computer'">
        <template #trail>
          <MButton variant="text" icon="arrow_upward" :disabled="!here || loading" @click="up">{{ C.browseUp }}</MButton>
        </template>
      </MItem>
    </MGroup>
    <MSkeleton v-if="loading" :rows="4" />
    <MBanner v-else-if="error" tone="error" icon="error">{{ error }}</MBanner>
    <MGroup v-else-if="entries.length">
      <MItem v-for="e in entries" :key="e.path" action class="spx-browse__row" :class="{ 'is-known': e.known }" :icon="iconOf(e)" :title="e.name" @click="open(e)">
        <template v-if="e.known" #trail><MIcon name="check_circle" class="spx-browse__known" /></template>
        <template v-else-if="e.kind !== 'file'" #trail><MIcon name="chevron_right" /></template>
      </MItem>
    </MGroup>
    <MEmpty v-else icon="folder_open" :title="C.browseEmpty" />
    <MFooter v-if="truncated">{{ C.browseMore(entries.length) }}</MFooter>
    <div class="spx-browse__actions">
      <MButton variant="text" @click="$emit('cancel')">{{ C.browseClose }}</MButton>
      <MButton v-if="kind === 'folder'" icon="check" :disabled="!here || loading" @click="$emit('choose', here)">{{ C.browseHere }}</MButton>
    </div>
  </MSection>
</template>

<script setup>
import { onMounted, ref } from 'vue'
import { MBanner, MButton, MEmpty, MFooter, MGroup, MIcon, MItem, MSection, MSkeleton } from 'foundation-ui/vue'
import { useSparxie } from '../../composables/useSparxie.js'
import { useSparxieCopy } from '../../composables/useSparxieCopy.js'
import { midCut } from '../../utils/sparxie/model.js'

// The picker of a browser: it cannot give the host a path, and the host never opens a dialog of its own (it is a service). The host lists its drives and folders as the signed-in person, read-only,
// and the page shows them here. `kind` 'folder' chooses the folder that is open; 'file' chooses an .exe, the known game executables first.
const props = defineProps({ kind: { type: String, default: 'file' }, start: { type: String, default: '' } })
const emit = defineEmits(['choose', 'cancel'])
const C = useSparxieCopy()
const { client } = useSparxie()
const here = ref('')
const parent = ref('')
const entries = ref([])
const truncated = ref(false)
const loading = ref(true)
const error = ref('')

const iconOf = (e) => (e.kind === 'drive' ? 'storage' : e.kind === 'dir' ? 'folder' : e.known ? 'sports_esports' : 'description')

async function go(path) {
  loading.value = true; error.value = ''
  try {
    const listed = await client().browse(path, { files: props.kind === 'file' })
    here.value = listed.path; parent.value = listed.parent; entries.value = listed.entries; truncated.value = listed.truncated
  } catch (e) {
    error.value = e?.code === 'no_user_session' ? C.value.noUser : C.value.browseFailed
  } finally { loading.value = false }
}
const open = (e) => { if (e.kind === 'file') emit('choose', e.path); else go(e.path) }
const up = () => go(parent.value)
onMounted(() => go(props.start))
</script>

<style scoped>
.spx-browse__row.is-known { font-weight: 600; }
.spx-browse__known { color: var(--md-sys-color-primary); }
.spx-browse__actions { display: flex; justify-content: flex-end; gap: 8px; margin-top: 12px; }
</style>
