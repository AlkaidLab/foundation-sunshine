<script setup>
import { ref } from 'vue'
import { useI18n } from 'vue-i18n'
import ConfirmDialog from './ConfirmDialog.vue'
import ResourceLink from './ResourceLink.vue'
import {
  HARMONY_CLIENT_URL,
  HOME_RESOURCE_GROUPS,
  LEGAL_RESOURCES,
  resolveResourceText,
} from '../../config/resources.js'
import { openExternalUrl } from '../../utils/helpers.js'

const { t } = useI18n()
const showHarmonyModal = ref(false)

const resourceTitle = (resource) => resolveResourceText(t, resource, 'title')
const resourceDescription = (resource) => resolveResourceText(t, resource, 'description')

const handleResourceActivate = (resource, event) => {
  if (resource.action !== 'harmony') return
  event.preventDefault()
  showHarmonyModal.value = true
}

const closeHarmonyModal = () => {
  showHarmonyModal.value = false
}

const confirmHarmonyLink = async () => {
  closeHarmonyModal()
  try {
    await openExternalUrl(HARMONY_CLIENT_URL)
  } catch (error) {
    console.error('Failed to open Harmony client URL:', error)
  }
}
</script>

<template>
  <div class="resource-section">
    <div class="resource-groups">
      <section
        v-for="group in HOME_RESOURCE_GROUPS"
        :key="group.id"
        class="resource-group"
      >
        <h6 class="resource-group-title">
          <i :class="group.icon" aria-hidden="true"></i>
          {{ $t(group.titleKey) }}
        </h6>
        <div class="resource-items">
          <div v-for="resource in group.items" :key="resource.id" class="resource-item">
            <ResourceLink
              compact
              :href="resource.href"
              :target="resource.action ? '' : '_blank'"
              :action="resource.action === 'harmony'"
              :title="resourceTitle(resource)"
              :description="resourceDescription(resource)"
              :icon="resource.icon"
              :variant="resource.variant"
              :arrow-icon="resource.arrowIcon"
              :arrow-class="resource.arrowClass"
              @activate="handleResourceActivate(resource, $event)"
            />
          </div>
        </div>
      </section>
    </div>

    <footer class="legal-footer" :aria-label="$t('resource_card.legal')">
      <div class="legal-footer-heading">
        <span class="legal-title">{{ $t('resource_card.legal') }}</span>
      </div>
      <p class="legal-description">{{ $t('resource_card.legal_desc') }}</p>

      <div class="legal-footer-meta">
        <span class="legal-license">GNU GPL v3.0</span>
        <nav class="legal-footer-links" :aria-label="$t('resource_card.legal')">
          <a
            v-for="resource in LEGAL_RESOURCES"
            :key="resource.id"
            class="legal-link"
            :href="resource.href"
            target="_blank"
            rel="noopener noreferrer"
            :aria-label="`${resourceTitle(resource)} (${$t('_common.opens_new_tab')})`"
            :title="resourceDescription(resource)"
          >
            {{ resourceTitle(resource) }}
          </a>
        </nav>
      </div>
    </footer>

    <ConfirmDialog
      :show="showHarmonyModal"
      dialog-id="harmony-client-link"
      :title="$t('resource_card.harmony_client')"
      title-icon="fas fa-mobile-alt"
      :close-label="$t('_common.close')"
      @close="closeHarmonyModal"
    >
      <p>{{ $t('setup.harmony_modal_link_notice') }}</p>
      <p>{{ $t('setup.harmony_modal_desc') }}</p>
      <template #actions>
        <button type="button" class="btn btn-secondary" @click="closeHarmonyModal">
          {{ $t('_common.cancel') }}
        </button>
        <button type="button" class="btn btn-primary" @click="confirmHarmonyLink">
          <i class="fas fa-external-link-alt me-1" aria-hidden="true"></i>
          {{ $t('setup.harmony_goto_repo') }}
        </button>
      </template>
    </ConfirmDialog>
  </div>
</template>

<style scoped>
.resource-section {
  display: flex;
  min-height: 0;
  flex: 1;
  flex-direction: column;
}

.resource-groups {
  display: grid;
  align-items: start;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 0.6rem;
}

.resource-group {
  min-width: 0;
  margin: 0;
  padding: 0.55rem 0.6rem 0.6rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  background: linear-gradient(
    150deg,
    color-mix(in srgb, var(--ui-surface-strong) 78%, transparent),
    color-mix(in srgb, var(--ui-surface) 68%, transparent)
  );
}

.resource-items {
  display: grid;
  grid-auto-rows: 3.4rem;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 0.6rem;
}

.resource-item {
  display: flex;
  min-width: 0;
}

.resource-group :deep(.resource-link) {
  width: 100%;
  height: 100%;
}

.resource-group-title {
  display: flex;
  align-items: center;
  gap: 0.35rem;
  margin: 0 0.1rem 0.45rem;
  color: var(--ui-text-primary);
  font-size: var(--font-size-sm);
  font-weight: 650;
  letter-spacing: 0.03em;
}

.resource-group-title i {
  display: inline-flex;
  width: 1.35rem;
  height: 1.35rem;
  align-items: center;
  justify-content: center;
  border-radius: 0.48rem;
  background: var(--ui-accent-soft);
  color: var(--ui-accent);
  font-size: 0.58rem;
}

.legal-footer {
  display: flex;
  width: 100%;
  align-items: center;
  gap: 0.2rem;
  margin-top: auto;
  padding: 0.85rem 0.2rem 0.55rem;
  flex-direction: column;
  color: var(--ui-text-primary);
  font-size: 0.7rem;
  line-height: 1.45;
  text-align: center;
}

.legal-footer-heading {
  display: flex;
  width: min(100%, 22rem);
  align-items: center;
  gap: 0.7rem;
}

.legal-footer-heading::before,
.legal-footer-heading::after {
  height: 1px;
  flex: 1;
  background: color-mix(in srgb, var(--ui-text-muted) 38%, transparent);
  content: '';
}

.legal-title {
  color: var(--ui-text-primary);
  font-size: 0.72rem;
  font-weight: 650;
  letter-spacing: 0.08em;
}

.legal-description {
  max-width: 44rem;
  margin: 0;
  color: var(--ui-text-primary);
}

.legal-footer-meta,
.legal-footer-links {
  display: flex;
  align-items: center;
  gap: 0.42rem 0.7rem;
  flex-wrap: wrap;
}

.legal-license {
  color: var(--ui-text-primary);
  font-weight: 600;
  letter-spacing: 0.03em;
}

.legal-footer-links::before {
  color: var(--ui-text-muted);
  content: '\00b7';
}

.legal-link {
  display: inline-flex;
  align-items: center;
  color: var(--ui-text-primary);
  text-decoration: none;
  transition: color 0.2s ease;
}

.legal-link + .legal-link::before {
  margin-right: 0.7rem;
  color: var(--ui-text-muted);
  content: '\00b7';
}

.legal-link:hover,
.legal-link:focus-visible {
  color: var(--ui-accent);
  text-decoration: underline;
  text-underline-offset: 0.18rem;
}

@media (max-width: 991.98px) {
  .resource-groups {
    grid-template-columns: 1fr;
  }
}

@media (max-width: 575.98px) {
  .resource-groups {
    display: block;
  }

  .resource-items {
    grid-auto-rows: minmax(3.25rem, auto);
    grid-template-columns: 1fr;
  }

  .resource-group + .resource-group {
    margin-top: 0.65rem;
  }
}
</style>
