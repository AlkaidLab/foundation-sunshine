<script setup>
import { ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { MButton, MClientTag, MFooter, MGroup, MIcon, MItem, MSection, MSheet } from 'foundation-ui/vue'
import ConfirmDialog from './ConfirmDialog.vue'
import { HARMONY_CLIENT_URL, HOME_RESOURCE_GROUPS, LEGAL_RESOURCES, resolveRandomResourceHref, resolveResourceHref, resolveResourceText } from '../../config/resources.js'
import { openExternalUrl } from '../../utils/helpers.js'

// The links the old home page listed (website, tutorial, community, clients, community projects, legal) as grouped rows.
const open = defineModel({ type: Boolean, default: false })
const { t, locale } = useI18n()
const harmony = ref(false)

// Material icon of each link; the catalogue in config/resources.js only has font-icon classes.
const ICONS = {
  'official-website': 'public', 'foundation-github': 'code', tutorial: 'book', 'community-group': 'forum',
  'android-vplus': 'android', 'harmony-vplus': 'devices', 'moonlight-desktop': 'desktop_windows', 'moonlight-macos': 'laptop_mac', voidlink: 'devices',
  'crown-edition': 'android', 'moonlight-ohos': 'devices', license: 'gavel', 'third-party-notice': 'description',
}
const title = (resource) => resolveResourceText(t, resource, 'title')
const text = (resource) => resolveResourceText(t, resource, 'description')
const fail = (error) => console.error('Failed to open resource URL:', error)
function activate(resource) {
  if (resource.action === 'harmony') { harmony.value = true; return }
  const href = resource.randomHrefs ? resolveRandomResourceHref(resource) : resolveResourceHref(resource, locale.value)
  openExternalUrl(href).catch(fail)
}
const confirmHarmony = () => { harmony.value = false; openExternalUrl(HARMONY_CLIENT_URL).catch(fail) }
</script>

<template>
  <MSheet v-model="open" :title="t('home.links_title')" side="end" :close-label="t('_common.close')">
    <div class="m-sheet-stack">
      <MSection v-for="group in HOME_RESOURCE_GROUPS" :key="group.id" :title="t(group.titleKey)">
        <MGroup>
          <MItem v-for="resource in group.items" :key="resource.id" action :icon="ICONS[resource.id] || 'open_in_new'" :title="title(resource)" :text="text(resource)" @click="activate(resource)">
            <template #trail><MClientTag v-if="resource.client" :official="resource.client === 'official'" :locale="String(locale)" /><MIcon name="open_in_new" /></template>
          </MItem>
        </MGroup>
      </MSection>
      <MSection :title="t('resource_card.legal')">
        <MGroup>
          <MItem v-for="resource in LEGAL_RESOURCES" :key="resource.id" action :icon="ICONS[resource.id]" :title="title(resource)" :text="text(resource)" @click="activate(resource)">
            <template #trail><MIcon name="open_in_new" /></template>
          </MItem>
        </MGroup>
        <MFooter>{{ t('resource_card.legal_desc') }} GNU GPL v3.0</MFooter>
      </MSection>
    </div>
  </MSheet>
  <ConfirmDialog :show="harmony" dialog-id="harmony-client-link" :title="t('resource_card.harmony_client')" title-icon="devices" :close-label="t('_common.close')" @close="harmony = false">
    <p>{{ t('setup.harmony_modal_link_notice') }}</p>
    <p>{{ t('setup.harmony_modal_desc') }}</p>
    <template #actions>
      <MButton variant="text" @click="harmony = false">{{ t('_common.cancel') }}</MButton>
      <MButton icon="open_in_new" @click="confirmHarmony">{{ t('setup.harmony_goto_repo') }}</MButton>
    </template>
  </ConfirmDialog>
</template>
