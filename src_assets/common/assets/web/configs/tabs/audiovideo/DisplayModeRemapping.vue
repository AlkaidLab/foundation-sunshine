<script setup>
import { computed, ref } from 'vue'
import { $tp } from '../../../platform-i18n'

const props = defineProps({
  config: Object,
  displayModeRemapping: Array,
})

const config = ref(props.config)
const displayModeRemapping = ref(props.displayModeRemapping || [])

const remappingType = computed(() => {
  if (config.value.resolution_change !== 'automatic') return 'refresh_rate_only'
  if (config.value.refresh_rate_change !== 'automatic') return 'resolution_only'
  return ''
})

const filteredRemappings = computed(() => {
  const entries = []
  displayModeRemapping.value.forEach((entry, index) => {
    if (entry.type === remappingType.value) entries.push({ entry, index })
  })
  return entries
})

function addRemapping() {
  displayModeRemapping.value.push({
    type: remappingType.value,
    received_resolution: '',
    received_fps: '',
    final_resolution: '',
    final_refresh_rate: '',
  })
}
</script>

<template>
  <details
    v-if="config.resolution_change === 'automatic' || config.refresh_rate_change === 'automatic'"
    class="display-mode-remapping"
  >
    <summary>
      <i class="fas fa-shuffle" aria-hidden="true"></i>
      {{ $tp('config.display_mode_remapping') }}
    </summary>
    <div class="remapping-content">
      <div class="form-text">
        <p class="pre-line">{{ $tp('config.display_mode_remapping_desc') }}</p>
        <p v-if="remappingType === ''" class="pre-line">
          {{ $tp('config.display_mode_remapping_default_mode_desc') }}
        </p>
        <p v-if="remappingType === 'resolution_only'" class="pre-line">
          {{ $tp('config.display_mode_remapping_resolution_only_mode_desc') }}
        </p>
      </div>

      <div v-if="filteredRemappings.length > 0" class="remapping-table-shell">
        <table class="table remapping-table">
          <thead>
            <tr>
              <th v-if="remappingType !== 'refresh_rate_only'" scope="col">
                {{ $tp('config.display_mode_remapping_received_resolution') }}
              </th>
              <th v-if="remappingType !== 'resolution_only'" scope="col">
                {{ $tp('config.display_mode_remapping_received_fps') }}
              </th>
              <th v-if="remappingType !== 'refresh_rate_only'" scope="col">
                {{ $tp('config.display_mode_remapping_final_resolution') }}
              </th>
              <th v-if="remappingType !== 'resolution_only'" scope="col">
                {{ $tp('config.display_mode_remapping_final_refresh_rate') }}
              </th>
              <th scope="col"></th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="{ entry, index } in filteredRemappings" :key="index">
              <template v-if="remappingType === ''">
                <td>
                  <input
                    v-model="entry.received_resolution"
                    type="text"
                    class="form-control monospace"
                    :placeholder="`1920x1080 (${$t('config.display_mode_remapping_optional')})`"
                  />
                </td>
                <td>
                  <input
                    v-model="entry.received_fps"
                    type="text"
                    class="form-control monospace"
                    :placeholder="`60 (${$t('config.display_mode_remapping_optional')})`"
                  />
                </td>
                <td>
                  <input
                    v-model="entry.final_resolution"
                    type="text"
                    class="form-control monospace"
                    :placeholder="`2560x1440 (${$t('config.display_mode_remapping_optional')})`"
                  />
                </td>
                <td>
                  <input
                    v-model="entry.final_refresh_rate"
                    type="text"
                    class="form-control monospace"
                    :placeholder="`119.95 (${$t('config.display_mode_remapping_optional')})`"
                  />
                </td>
              </template>
              <template v-else-if="remappingType === 'resolution_only'">
                <td>
                  <input v-model="entry.received_resolution" type="text" class="form-control monospace" placeholder="1920x1080" />
                </td>
                <td>
                  <input v-model="entry.final_resolution" type="text" class="form-control monospace" placeholder="2560x1440" />
                </td>
              </template>
              <template v-else>
                <td>
                  <input v-model="entry.received_fps" type="text" class="form-control monospace" placeholder="60" />
                </td>
                <td>
                  <input v-model="entry.final_refresh_rate" type="text" class="form-control monospace" placeholder="119.95" />
                </td>
              </template>
              <td>
                <button
                  type="button"
                  class="remapping-delete"
                  :aria-label="$t('_common.delete')"
                  :title="$t('_common.delete')"
                  @click="displayModeRemapping.splice(index, 1)"
                >
                  <i class="fas fa-trash"></i>
                </button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>

      <button type="button" class="remapping-add mt-2 btn btn-primary" @click="addRemapping">
        &plus; {{ $t('config.add') }}
      </button>
    </div>
  </details>
</template>

<style scoped>
.display-mode-remapping {
  margin-bottom: 1rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-sm);
  background: var(--ui-surface);
}

.display-mode-remapping summary {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.75rem 0.85rem;
  color: var(--ui-text-primary);
  font-size: 0.88rem;
  font-weight: 600;
  cursor: pointer;
  list-style: none;
}

.display-mode-remapping summary::-webkit-details-marker {
  display: none;
}

.display-mode-remapping summary i {
  color: var(--ui-accent);
}

.display-mode-remapping summary::after {
  margin-left: auto;
  font-family: 'Font Awesome 7 Free';
  font-weight: 900;
  content: '\f078';
  transition: transform 0.2s ease;
}

.display-mode-remapping[open] summary::after {
  transform: rotate(180deg);
}

.remapping-content {
  padding: 0 0.85rem 0.85rem;
}

.pre-line {
  white-space: pre-line;
}

.remapping-table-shell {
  margin-top: 0.75rem;
  overflow-x: auto;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-surface);
}

.remapping-table {
  min-width: 720px;
  margin: 0;
  --bs-table-bg: transparent;
  --bs-table-color: var(--ui-text-secondary);
  --bs-table-border-color: var(--ui-border);
}

.remapping-table th {
  padding: 0.75rem;
  background: var(--ui-surface-strong);
  color: var(--ui-text-primary);
  font-size: 0.82rem;
  font-weight: 600;
  vertical-align: middle;
}

.remapping-table td {
  min-width: 150px;
  padding: 0.65rem;
  vertical-align: middle;
}

.remapping-table td:last-child,
.remapping-table th:last-child {
  width: 52px;
  min-width: 52px;
  text-align: center;
}

.monospace {
  font-family: 'SF Mono', 'Cascadia Code', Consolas, monospace;
}

.remapping-delete {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 2.1rem;
  height: 2.1rem;
  padding: 0;
  border: 1px solid var(--ui-danger-border);
  border-radius: var(--ui-radius-sm);
  background: transparent;
  color: var(--ui-danger-text);
}

.remapping-delete:hover,
.remapping-delete:focus-visible {
  background: var(--ui-danger-soft);
  box-shadow: 0 0 0 3px var(--ui-danger-soft);
}

@media (max-width: 575.98px) {
  .remapping-table {
    min-width: 640px;
  }

  .remapping-add {
    width: 100%;
  }
}
</style>
