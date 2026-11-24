<script setup>
import { onMounted, ref, computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { MButton, MChoiceItem, MEmpty, MFooter, MGroup, MBanner, MSection, MSkeleton, MSnackbar } from 'foundation-ui/vue'
import CfgSub from '../../../components/cfg/CfgSub.vue'

// The gamepad type of one game (an application): follows the global choice, or a type of its own. The list is the
// application list of the host; a change is saved at once, the way the applications page saves it.
const props = defineProps({ options: { type: Array, required: true } })
const { t } = useI18n()
const apps = ref([])
const loading = ref(true)
const failed = ref(false)
const saving = ref(-1)
const note = ref('')
const snack = ref(false)
const choices = computed(() => [{ value: '', label: t('ds5.pergame.follow') }, ...props.options])

const load = async () => {
  loading.value = true
  failed.value = false
  try {
    const response = await fetch('/api/apps', { headers: { Accept: 'application/json' } })
    if (!response.ok) throw new Error(`HTTP ${response.status}`)
    const body = await response.json()
    apps.value = Array.isArray(body) ? body : body?.apps || []
  } catch {
    failed.value = true
  } finally {
    loading.value = false
  }
}
const say = (text) => { note.value = text; snack.value = true }
const save = async (index, value) => {
  const app = apps.value[index]
  if (!app || saving.value !== -1) return
  const previous = app.gamepad || ''
  app.gamepad = value
  saving.value = index
  try {
    const response = await fetch('/api/apps', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', Accept: 'application/json' },
      body: JSON.stringify({ apps: apps.value, editApp: { index, ...app } }),
    })
    if (response.status === 409) { app.gamepad = previous; say(t('ds5.pergame.conflict')); await load(); return }
    if (!response.ok) throw new Error(`HTTP ${response.status}`)
    say(t('ds5.pergame.saved'))
  } catch {
    app.gamepad = previous
    say(t('ds5.pergame.failed'))
  } finally {
    saving.value = -1
  }
}
onMounted(load)
</script>

<template>
  <CfgSub id="gamepad-pergame" :title="t('ds5.pergame.title')">
    <MSkeleton v-if="loading" :rows="3" />
    <MBanner v-else-if="failed" tone="error" icon="error">
      {{ t('ds5.pergame.load_failed') }}
      <template #actions><MButton variant="text" size="sm" @click="load">{{ t('ds5.retry') }}</MButton></template>
    </MBanner>
    <MEmpty v-else-if="!apps.length" icon="apps" :title="t('ds5.pergame.empty')" />
    <MSection v-else>
      <MGroup>
        <MChoiceItem v-for="(app, index) in apps" :key="app.name + index" :model-value="app.gamepad || ''" :title="app.name" :options="choices" :disabled="saving === index" :cancel-label="t('_common.cancel')" @update:model-value="(value) => save(index, value)" />
      </MGroup>
      <MFooter>{{ t('ds5.pergame.hint') }}</MFooter>
    </MSection>
    <MSnackbar v-model="snack" :message="note" />
  </CfgSub>
</template>
