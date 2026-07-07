<template>
  <div class="page-plugins">
    <Navbar />
    <div class="container py-4">
      <div class="d-flex align-items-center justify-content-between gap-3 mb-4">
        <h1 class="h3 page-title mb-0">
          <i class="fas fa-puzzle-piece me-2"></i>
          {{ $t('plugins.title') }}
        </h1>
        <button class="btn btn-outline-secondary btn-sm" :disabled="isCurrentTabLoading" @click="reloadCurrentTab">
          <i class="fas fa-sync-alt me-1" :class="{ 'fa-spin': isCurrentTabLoading }"></i>
          {{ $t('plugins.reload') }}
        </button>
      </div>

      <div class="nav nav-tabs plugin-tabs mb-3" role="tablist">
        <button
          class="nav-link"
          :class="{ active: activeTab === 'installed' }"
          type="button"
          role="tab"
          @click="activeTab = 'installed'"
        >
          <i class="fas fa-box-open me-1"></i>{{ $t('plugins.tabs_installed') }}
        </button>
        <button
          class="nav-link"
          :class="{ active: activeTab === 'marketplace' }"
          type="button"
          role="tab"
          @click="showMarketplace"
        >
          <i class="fas fa-store me-1"></i>{{ $t('plugins.tabs_marketplace') }}
        </button>
      </div>

      <div v-if="activeTab === 'installed' && error" class="alert alert-danger">
        <i class="fas fa-exclamation-circle me-2"></i>{{ error }}
      </div>

      <template v-if="activeTab === 'installed'">
        <div v-if="loading && plugins.length === 0" class="text-muted py-4">
          <i class="fas fa-circle-notch fa-spin me-2"></i>{{ $t('plugins.loading') }}
        </div>

        <div v-else-if="plugins.length === 0" class="alert alert-secondary">
          {{ $t('plugins.none') }}
        </div>

        <div v-else class="plugin-list">
          <section v-for="plugin in plugins" :key="plugin.id" class="plugin-panel">
            <div class="plugin-main">
              <div class="plugin-title">
                <div>
                  <h2 class="h5 mb-1">{{ plugin.name || plugin.id }}</h2>
                  <div class="text-muted small">{{ plugin.id }} <span v-if="plugin.version">v{{ plugin.version }}</span></div>
                </div>
                <span class="badge" :class="statusClass(plugin)">{{ $t(statusKey(plugin)) }}</span>
              </div>

              <div class="plugin-actions">
                <div class="form-check form-switch plugin-toggle">
                  <input
                    class="form-check-input"
                    type="checkbox"
                    role="switch"
                    :id="`enabled-${plugin.id}`"
                    v-model="plugin.enabled"
                    :disabled="isSaving(plugin.id)"
                    @change="saveEnabled(plugin)"
                  />
                  <label class="form-check-label" :for="`enabled-${plugin.id}`">{{ $t('_common.enabled') }}</label>
                </div>
                <button
                  v-for="action in plugin.actions"
                  :key="action.id"
                  class="btn btn-sm"
                  :class="{ 'btn-outline-danger': action.danger, 'btn-outline-primary': !action.danger }"
                  :disabled="isRunning(actionKey(plugin, action)) || !canRunPlugin(plugin)"
                  @click="runAction(plugin, action)"
                >
                  <i class="fas me-1" :class="isRunning(actionKey(plugin, action)) ? 'fa-circle-notch fa-spin' : (action.icon || 'fa-play')"></i>
                  {{ action.title || action.id }}
                </button>
              </div>
            </div>

            <div class="plugin-meta">
              <span v-for="slot in plugin.slots" :key="slot" class="badge text-bg-light">{{ slot }}</span>
            </div>

            <div v-if="plugin.capabilities?.length" class="plugin-meta capabilities">
              <span v-for="capability in plugin.capabilities" :key="capability" class="badge text-bg-secondary">
                {{ capability }}
              </span>
            </div>

            <div class="plugin-run">
              <span class="text-muted small">{{ $t('plugins.last_run') }}:</span>
              <template v-if="plugin.last_run">
                <span class="badge" :class="runStatusClass(plugin.last_run)">
                  {{ $t(runStatusKey(plugin.last_run)) }}
                </span>
                <span class="small text-muted">
                  {{ formatTime(plugin.last_run.finished_at_ms || plugin.last_run.started_at_ms) }}
                  <span class="separator">&middot;</span> {{ plugin.last_run.action_id || plugin.last_run.event }}
                  <span class="separator">&middot;</span> {{ formatDuration(plugin.last_run) }}
                </span>
              </template>
              <span v-else class="small text-muted">{{ $t('plugins.never_run') }}</span>
            </div>

            <div v-if="status[plugin.id]" class="small text-success mt-2">{{ $t(status[plugin.id]) }}</div>

            <details v-if="plugin.history?.length" class="plugin-history">
              <summary class="small text-muted">{{ $t('plugins.recent_runs') }}</summary>
              <div v-for="run in plugin.history" :key="`${run.started_at_ms}-${run.event}`" class="history-row">
                <span class="badge" :class="runStatusClass(run)">{{ $t(runStatusKey(run)) }}</span>
                <span class="small">{{ formatTime(run.finished_at_ms || run.started_at_ms) }}</span>
                <span class="small text-muted">{{ run.action_id || run.event }}</span>
                <span class="small text-muted">{{ formatDuration(run) }}</span>
                <div v-if="runDetail(run)" class="history-detail small text-muted">{{ runDetail(run) }}</div>
              </div>
            </details>

            <div v-if="schemaEntries(plugin).length" class="plugin-config">
              <h3 class="h6">{{ $t('plugins.configuration') }}</h3>
              <div class="row g-3">
                <div v-for="[key, field] in schemaEntries(plugin)" :key="key" class="col-md-6">
                  <div v-if="field.type === 'boolean'" class="form-check form-switch">
                    <input
                      class="form-check-input"
                      type="checkbox"
                      role="switch"
                      :id="`${plugin.id}-${key}`"
                      v-model="plugin.config[key]"
                    />
                    <label class="form-check-label" :for="`${plugin.id}-${key}`">
                      {{ field.title || key }}
                    </label>
                  </div>
                  <label v-else class="form-label small" :for="`${plugin.id}-${key}`">
                    {{ field.title || key }}
                    <input
                      class="form-control form-control-sm mt-1"
                      :id="`${plugin.id}-${key}`"
                      v-model="plugin.config[key]"
                    />
                  </label>
                </div>
              </div>

              <div class="d-flex align-items-center gap-2 mt-3">
                <button class="btn btn-primary btn-sm" :disabled="isSaving(plugin.id)" @click="saveConfig(plugin)">
                  <i class="fas fa-save me-1"></i>{{ $t('_common.save') }}
                </button>
              </div>
            </div>
          </section>
        </div>
      </template>

      <template v-else>
        <div v-if="marketplaceError" class="alert alert-warning">
          <i class="fas fa-triangle-exclamation me-2"></i>{{ marketplaceError }}
        </div>

        <div v-if="marketplaceLoading && marketplacePlugins.length === 0" class="text-muted py-4">
          <i class="fas fa-circle-notch fa-spin me-2"></i>{{ $t('plugins.marketplace_loading') }}
        </div>

        <div v-else-if="marketplacePlugins.length === 0 && !marketplaceError" class="alert alert-secondary">
          {{ $t('plugins.marketplace_none') }}
        </div>

        <div v-else class="plugin-list">
          <section v-for="plugin in marketplacePlugins" :key="plugin.id" class="plugin-panel">
            <div class="plugin-main">
              <div class="plugin-title">
                <div>
                  <h2 class="h5 mb-1">{{ plugin.name || plugin.id }}</h2>
                  <div class="text-muted small">{{ plugin.id }}</div>
                </div>
                <span class="badge" :class="marketplaceStatusClass(plugin)">{{ $t(marketplaceStatusKey(plugin)) }}</span>
              </div>

              <div class="plugin-actions">
                <button class="btn btn-outline-primary btn-sm" :disabled="!releaseUrl(plugin)" @click="openRelease(plugin)">
                  <i class="fas fa-arrow-up-right-from-square me-1"></i>{{ $t('plugins.marketplace_open_release') }}
                </button>
              </div>
            </div>

            <p v-if="plugin.summary" class="plugin-summary mb-0">{{ plugin.summary }}</p>

            <div class="plugin-meta">
              <span v-for="platform in plugin.platforms" :key="platform" class="badge text-bg-light">{{ platform }}</span>
              <span v-if="plugin.latest_release?.version" class="badge text-bg-light">v{{ plugin.latest_release.version }}</span>
              <span v-if="plugin.installed_version" class="badge text-bg-success">{{ $t('plugins.installed_version') }} {{ plugin.installed_version }}</span>
            </div>

            <div v-if="plugin.capabilities?.length" class="plugin-meta capabilities">
              <span v-for="capability in plugin.capabilities" :key="capability" class="badge text-bg-secondary">
                {{ capability }}
              </span>
            </div>

            <div v-if="plugin.reason" class="small text-muted mt-2">{{ plugin.reason }}</div>
          </section>
        </div>

        <div v-if="marketplaceSource" class="small text-muted mt-3">
          {{ $t('plugins.marketplace_source') }}: {{ marketplaceSource }}
        </div>
      </template>
    </div>
  </div>
</template>

<script setup>
import { computed, onMounted, ref } from 'vue'
import Navbar from '../components/layout/Navbar.vue'

const activeTab = ref('installed')
const plugins = ref([])
const marketplacePlugins = ref([])
const marketplaceSource = ref('')
const marketplaceLoaded = ref(false)
const loading = ref(false)
const marketplaceLoading = ref(false)
const error = ref('')
const marketplaceError = ref('')
const saving = ref({})
const running = ref({})
const status = ref({})

const isCurrentTabLoading = computed(() => activeTab.value === 'marketplace' ? marketplaceLoading.value : loading.value)

const setSaving = (id, value) => {
  saving.value = { ...saving.value, [id]: value }
}

const isSaving = (id) => saving.value[id] === true

const setRunning = (id, value) => {
  running.value = { ...running.value, [id]: value }
}

const isRunning = (id) => running.value[id] === true

const normalizePlugin = (plugin) => ({
  ...plugin,
  config: { ...(plugin.config || {}) },
  slots: plugin.slots || [],
  actions: plugin.actions || [],
  capabilities: plugin.capabilities || [],
  permissions: plugin.permissions || [],
  history: plugin.history || [],
  last_run: plugin.last_run || null,
})

const normalizeMarketplacePlugin = (plugin) => ({
  ...plugin,
  platforms: plugin.platforms || [],
  capabilities: plugin.capabilities || [],
  tags: plugin.tags || [],
  latest_release: plugin.latest_release || null,
})

const loadPlugins = async () => {
  loading.value = true
  error.value = ''
  try {
    const response = await fetch('/api/plugins')
    const data = await response.json()
    if (!response.ok) {
      throw new Error(data.error || `HTTP ${response.status}`)
    }
    plugins.value = (data.plugins || []).map(normalizePlugin)
  } catch (err) {
    error.value = err.message
  } finally {
    loading.value = false
  }
}

const loadMarketplace = async () => {
  marketplaceLoading.value = true
  marketplaceError.value = ''
  try {
    const response = await fetch('/api/plugins/marketplace')
    const data = await response.json()
    if (!response.ok || data.status === false) {
      throw new Error(data.error || `HTTP ${response.status}`)
    }
    marketplacePlugins.value = (data.plugins || []).map(normalizeMarketplacePlugin)
    marketplaceSource.value = data.registry_url || data.source || ''
    marketplaceLoaded.value = true
  } catch (err) {
    marketplaceError.value = err.message
  } finally {
    marketplaceLoading.value = false
  }
}

const showMarketplace = async () => {
  activeTab.value = 'marketplace'
  if (!marketplaceLoaded.value) {
    await loadMarketplace()
  }
}

const reloadCurrentTab = () => {
  if (activeTab.value === 'marketplace') {
    marketplaceLoaded.value = false
    loadMarketplace()
    return
  }
  loadPlugins()
}

const schemaEntries = (plugin) => Object.entries(plugin.schema?.properties || {})

const statusKey = (plugin) => {
  if (!plugin.enabled) return 'plugins.status_disabled'
  if (!plugin.platform_supported) return 'plugins.status_unsupported'
  if (!plugin.entry_exists) return 'plugins.status_missing_entry'
  return 'plugins.status_ready'
}

const statusClass = (plugin) => {
  if (!plugin.enabled) return 'text-bg-secondary'
  if (!plugin.platform_supported || !plugin.entry_exists) return 'text-bg-warning'
  return 'text-bg-success'
}

const marketplaceStatusKey = (plugin) => {
  if (plugin.status === 'blocked') return 'plugins.status_blocked'
  if (plugin.status === 'deprecated') return 'plugins.status_deprecated'
  if (plugin.installed) return 'plugins.status_installed'
  if (!plugin.platform_supported) return 'plugins.status_unsupported'
  return 'plugins.status_listed'
}

const marketplaceStatusClass = (plugin) => {
  if (plugin.status === 'blocked') return 'text-bg-danger'
  if (plugin.status === 'deprecated') return 'text-bg-warning'
  if (plugin.installed) return 'text-bg-success'
  if (!plugin.platform_supported) return 'text-bg-warning'
  return 'text-bg-primary'
}

const actionKey = (plugin, action) => `${plugin.id}:${action.id}`

const canRunPlugin = (plugin) => plugin.enabled && plugin.platform_supported && plugin.entry_exists

const runStatusKey = (run) => {
  if (!run) return 'plugins.status_unknown'
  if (run.status === 'success') return 'plugins.run_success'
  if (run.status === 'timeout') return 'plugins.run_timeout'
  if (run.status === 'start_error') return 'plugins.run_start_error'
  if (run.status === 'failed') return 'plugins.run_failed'
  return 'plugins.status_unknown'
}

const runStatusClass = (run) => {
  if (!run) return 'text-bg-secondary'
  if (run.status === 'success') return 'text-bg-success'
  if (run.status === 'timeout' || run.status === 'start_error') return 'text-bg-danger'
  if (run.status === 'failed') return 'text-bg-warning'
  return 'text-bg-secondary'
}

const formatTime = (ms) => {
  if (!ms) return '-'
  return new Date(ms).toLocaleString()
}

const formatDuration = (run) => {
  if (!Number.isFinite(run?.duration_ms)) return '-'
  return `${run.duration_ms} ms`
}

const runDetail = (run) => {
  if (!run) return ''
  const parts = []
  if (run.result?.stage) parts.push(`stage: ${run.result.stage}`)
  if (Number.isFinite(run.exit_code)) parts.push(`exit: ${run.exit_code}`)
  if (run.message) parts.push(run.message)
  else if (run.error) parts.push(run.error)
  else if (run.result_error) parts.push(run.result_error)
  if (run.output) parts.push(String(run.output).replace(/\s+/g, ' ').trim())
  return parts.filter(Boolean).join(' | ')
}

const releaseUrl = (plugin) => plugin.latest_release?.release_url || plugin.homepage || plugin.repo || ''

const openRelease = (plugin) => {
  const url = releaseUrl(plugin)
  if (url) {
    window.open(url, '_blank', 'noopener,noreferrer')
  }
}

const saveEnabled = async (plugin) => {
  setSaving(plugin.id, true)
  error.value = ''
  status.value = { ...status.value, [plugin.id]: '' }
  try {
    const response = await fetch(`/api/plugins/${encodeURIComponent(plugin.id)}/enabled`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ enabled: plugin.enabled }),
    })
    const data = await response.json()
    if (!response.ok || data.status === false) {
      throw new Error(data.error || `HTTP ${response.status}`)
    }
    status.value = { ...status.value, [plugin.id]: 'plugins.saved' }
    await loadPlugins()
  } catch (err) {
    plugin.enabled = !plugin.enabled
    error.value = err.message
  } finally {
    setSaving(plugin.id, false)
  }
}

const saveConfig = async (plugin) => {
  setSaving(plugin.id, true)
  error.value = ''
  status.value = { ...status.value, [plugin.id]: '' }
  try {
    const response = await fetch(`/api/plugins/${encodeURIComponent(plugin.id)}/config`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ config: plugin.config }),
    })
    const data = await response.json()
    if (!response.ok || data.status === false) {
      throw new Error(data.error || `HTTP ${response.status}`)
    }
    status.value = { ...status.value, [plugin.id]: 'plugins.saved' }
    await loadPlugins()
  } catch (err) {
    error.value = err.message
  } finally {
    setSaving(plugin.id, false)
  }
}

const runAction = async (plugin, action) => {
  const key = actionKey(plugin, action)
  setRunning(key, true)
  error.value = ''
  status.value = { ...status.value, [plugin.id]: '' }
  try {
    const response = await fetch(`/api/plugins/${encodeURIComponent(plugin.id)}/actions/${encodeURIComponent(action.id)}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({}),
    })
    const data = await response.json()
    if (!response.ok) {
      throw new Error(data.error || `HTTP ${response.status}`)
    }
    status.value = { ...status.value, [plugin.id]: data.status === false ? 'plugins.run_failed' : 'plugins.ran' }
    await loadPlugins()
  } catch (err) {
    error.value = err.message
  } finally {
    setRunning(key, false)
  }
}

onMounted(loadPlugins)
</script>

<style scoped>
.plugin-tabs {
  gap: 4px;
}

.plugin-list {
  display: grid;
  gap: 12px;
}

.plugin-panel {
  border: 1px solid var(--bs-border-color);
  border-radius: 8px;
  padding: 16px;
  background: var(--bs-body-bg);
}

.plugin-main {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 16px;
}

.plugin-title {
  display: flex;
  align-items: center;
  gap: 12px;
  min-width: 0;
}

.plugin-toggle {
  min-width: 110px;
}

.plugin-actions {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: 10px;
}

.plugin-summary {
  color: var(--bs-secondary-color);
  margin-top: 10px;
}

.plugin-meta {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-top: 12px;
}

.capabilities {
  opacity: 0.85;
}

.plugin-run {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 8px;
  margin-top: 12px;
}

.plugin-history {
  margin-top: 8px;
}

.history-row {
  display: grid;
  grid-template-columns: minmax(78px, auto) minmax(155px, auto) minmax(180px, 1fr) minmax(60px, auto);
  gap: 8px;
  align-items: center;
  padding: 4px 0;
}

.history-detail {
  grid-column: 1 / -1;
  overflow-wrap: anywhere;
}

.plugin-config {
  border-top: 1px solid var(--bs-border-color);
  margin-top: 14px;
  padding-top: 14px;
}

@media (max-width: 640px) {
  .plugin-main,
  .plugin-title {
    align-items: flex-start;
    flex-direction: column;
  }

  .plugin-actions {
    justify-content: flex-start;
  }

  .history-row {
    grid-template-columns: 1fr;
    gap: 2px;
  }
}
</style>
