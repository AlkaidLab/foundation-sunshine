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
    <div class="card shadow-sm resource-library-card">
      <div class="card-header resource-card-header">
        <h5 class="card-title mb-0">
          <i class="fas fa-book-open me-2" aria-hidden="true"></i>
          {{ $t('resource_card.resources') }}
        </h5>
      </div>
      <div class="card-body">
        <div class="resource-groups">
          <section
            v-for="group in HOME_RESOURCE_GROUPS"
            :key="group.id"
            class="resource-group"
            :class="`resource-group--${group.id}`"
          >
            <h6 class="resource-group-title">
              <i :class="group.icon" class="me-2" aria-hidden="true"></i>
              {{ $t(group.titleKey) }}
            </h6>
            <div class="row">
              <div v-for="resource in group.items" :key="resource.id" :class="group.itemClass">
                <ResourceLink
                  compact
                  :href="resource.href"
                  :target="resource.action ? '' : '_blank'"
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
      </div>
    </div>

    <footer class="legal-footer">
      <div class="legal-footer-copy">
        <span class="legal-title">
          <i class="fas fa-gavel" aria-hidden="true"></i>
          {{ $t('resource_card.legal') }}
        </span>
        <span class="legal-description">{{ $t('resource_card.legal_desc') }}</span>
      </div>

      <nav class="legal-footer-links" :aria-label="$t('resource_card.legal')">
        <span class="legal-license">
          <i class="fas fa-balance-scale" aria-hidden="true"></i>
          GNU GPL v3.0
        </span>
        <a
          v-for="resource in LEGAL_RESOURCES"
          :key="resource.id"
          class="legal-link"
          :href="resource.href"
          target="_blank"
          rel="noopener noreferrer"
          :title="resourceDescription(resource)"
        >
          {{ resourceTitle(resource) }}
          <i class="fas fa-external-link-alt" aria-hidden="true"></i>
        </a>
      </nav>
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
  flex-direction: column;
}

.resource-library-card {
  overflow: hidden;
  border-radius: calc(var(--ui-radius-lg) + 1px);
}

.resource-card-header {
  position: relative;
  padding: 0.55rem 0.7rem;
  background: linear-gradient(
    135deg,
    color-mix(in srgb, var(--ui-surface-strong) 88%, var(--ui-accent-soft)),
    var(--ui-surface-strong)
  );
}

.resource-card-header::after {
  position: absolute;
  top: 50%;
  right: 0.85rem;
  width: 0.34rem;
  height: 0.34rem;
  border-radius: 50%;
  background: var(--ui-accent);
  box-shadow: -0.65rem 0 0 -0.05rem var(--ui-accent-secondary), -1.25rem 0 0 -0.1rem var(--ui-accent-soft);
  content: '';
  opacity: 0.55;
  transform: translateY(-50%);
}

.resource-card-header .card-title {
  display: flex;
  align-items: center;
  color: var(--ui-text-primary);
  font-size: 0.88rem;
}

.resource-card-header .card-title i {
  display: inline-flex;
  width: 1.65rem;
  height: 1.65rem;
  align-items: center;
  justify-content: center;
  border-radius: 0.58rem;
  background: var(--ui-accent-soft);
  font-size: 0.72rem;
}

.resource-section > .card > .card-body {
  padding: 0.42rem;
}

.resource-groups {
  display: grid;
  grid-template-columns: repeat(12, minmax(0, 1fr));
  gap: 0.42rem;
}

.resource-group {
  min-width: 0;
  margin: 0;
  padding: 0.38rem 0.42rem 0.42rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  background: linear-gradient(
    150deg,
    color-mix(in srgb, var(--ui-surface-strong) 78%, transparent),
    color-mix(in srgb, var(--ui-surface) 68%, transparent)
  );
}

.resource-group--official,
.resource-group--quick-start {
  grid-column: span 6;
}

.resource-group--clients {
  grid-column: span 8;
}

.resource-group--community {
  grid-column: span 4;
}

.resource-group .row {
  --bs-gutter-x: 0.42rem;
  --bs-gutter-y: 0.42rem;
}

.resource-group-title {
  display: flex;
  align-items: center;
  gap: 0.35rem;
  margin: 0 0.1rem 0.32rem;
  color: var(--ui-text-primary);
  font-size: 0.7rem;
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
  font-size: 0.58rem;
}

.resource-card-header .card-title i,
.resource-group-title i {
  color: var(--ui-accent);
}

.legal-footer {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 0.75rem 1.25rem;
  margin-top: auto;
  padding: 0.55rem 0.65rem 0.35rem;
  border-top: 1px solid color-mix(in srgb, var(--ui-border) 72%, transparent);
  background: linear-gradient(
    90deg,
    transparent,
    color-mix(in srgb, var(--ui-surface-strong) 52%, transparent) 8%,
    color-mix(in srgb, var(--ui-surface-strong) 52%, transparent) 92%,
    transparent
  );
  color: var(--ui-text-secondary);
  backdrop-filter: blur(6px);
  font-size: 0.68rem;
}

.legal-footer-copy,
.legal-footer-links {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  min-width: 0;
}

.legal-title,
.legal-license {
  display: inline-flex;
  flex: 0 0 auto;
  align-items: center;
  gap: 0.32rem;
  font-weight: 650;
}

.legal-title {
  color: var(--ui-text-primary);
}

.legal-title i,
.legal-license i {
  color: var(--ui-accent);
}

.legal-description {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.legal-license {
  color: var(--ui-text-secondary);
}

.legal-link {
  display: inline-flex;
  align-items: center;
  gap: 0.25rem;
  color: var(--ui-text-secondary);
  text-decoration: none;
  transition: color 0.2s ease;
}

.legal-link:hover,
.legal-link:focus-visible {
  color: var(--ui-accent);
  text-decoration: underline;
  text-underline-offset: 0.18rem;
}

.legal-link i {
  font-size: 0.52rem;
}

@media (max-width: 991.98px) {
  .resource-group--official,
  .resource-group--quick-start,
  .resource-group--clients,
  .resource-group--community {
    grid-column: span 12;
  }

  .legal-footer {
    align-items: flex-start;
    flex-direction: column;
    gap: 0.35rem;
  }

  .legal-footer-links {
    flex-wrap: wrap;
  }
}

@media (max-width: 575.98px) {
  .resource-groups {
    display: block;
  }

  .resource-group + .resource-group {
    margin-top: 0.65rem;
  }
}
</style>
