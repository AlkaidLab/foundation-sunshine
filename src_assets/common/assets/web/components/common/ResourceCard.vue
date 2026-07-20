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
    <div class="card shadow-sm">
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

    <div class="card shadow-sm mt-2 legal-card">
      <div class="card-body resource-legal-body">
        <div class="legal-summary">
          <h5 class="legal-title">
            <i class="fas fa-gavel me-2" aria-hidden="true"></i>
            {{ $t('resource_card.legal') }}
          </h5>
          <p class="legal-copy">{{ $t('resource_card.legal_desc') }}</p>
        </div>

        <div class="gpl-badge">
          <div class="d-flex align-items-center justify-content-center">
            <span class="badge-gpl">
              <i class="fas fa-balance-scale me-2" aria-hidden="true"></i>
              GNU General Public License v3.0
            </span>
          </div>
          <p class="text-center mt-2 mb-0">
            {{ $t('resource_card.gpl_license_text_1') }}
            <br />
            {{ $t('resource_card.gpl_license_text_2') }}
          </p>
        </div>

        <div class="row legal-links">
          <div v-for="resource in LEGAL_RESOURCES" :key="resource.id" class="col-md-6">
            <ResourceLink
              compact
              :href="resource.href"
              :title="resourceTitle(resource)"
              :description="resourceDescription(resource)"
              :icon="resource.icon"
              :variant="resource.variant"
            />
          </div>
        </div>
      </div>
    </div>

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
.resource-section > .card > .card-body {
  padding: 0.5rem;
}

.resource-groups {
  display: grid;
  grid-template-columns: repeat(12, minmax(0, 1fr));
  gap: 0.5rem;
}

.resource-group {
  min-width: 0;
  margin: 0;
  padding: 0.4rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-sm);
  background: color-mix(in srgb, var(--ui-surface) 70%, transparent);
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

.resource-group .row,
.legal-links {
  --bs-gutter-x: 0.5rem;
  --bs-gutter-y: 0.5rem;
}

.resource-group-title {
  margin-bottom: 0.35rem;
  color: var(--ui-text-secondary);
  font-size: 0.75rem;
  font-weight: 600;
  letter-spacing: 0.5px;
  text-transform: uppercase;
}

.resource-card-header .card-title i,
.resource-group-title i,
.legal-title i {
  color: var(--ui-accent);
}

.resource-legal-body {
  display: grid;
  grid-template-columns: minmax(0, 1.1fr) auto minmax(20rem, 1.15fr);
  align-items: center;
  gap: 0.65rem;
  padding: 0.5rem 0.65rem !important;
}

.legal-title {
  margin: 0 0 0.25rem;
  color: var(--ui-text-primary);
  font-size: 0.9rem;
  font-weight: 600;
}

.legal-copy {
  margin: 0;
  color: var(--ui-text-secondary);
  font-size: 0.78rem;
  line-height: 1.35;
}

.gpl-badge {
  min-width: 16rem;
}

.badge-gpl {
  display: inline-flex;
  align-items: center;
  padding: 0.4rem 0.7rem;
  border: 1px solid var(--ui-border-strong);
  border-radius: 50px;
  background: var(--ui-accent-soft);
  color: var(--ui-accent);
  font-size: 0.72rem;
  font-weight: 700;
  letter-spacing: 0.5px;
}

.gpl-badge p {
  color: var(--ui-text-muted);
  font-size: 0.65rem;
  line-height: 1.25;
}

.legal-links {
  margin: 0;
}

@media (max-width: 991.98px) {
  .resource-group--official,
  .resource-group--quick-start,
  .resource-group--clients,
  .resource-group--community {
    grid-column: span 12;
  }

  .resource-legal-body {
    grid-template-columns: 1fr;
  }

  .gpl-badge {
    min-width: 0;
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
