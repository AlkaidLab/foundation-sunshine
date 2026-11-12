<script setup>
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { MButton, MChip, MDialog, MField, MGroup, MIconButton, MItem, MSection, MSheet } from 'foundation-ui/vue'
import { SETTING_META, isChanged, settingsHash } from 'foundation-ui/settings/index.js'
import { browsePath } from '../../utils/browsePath.js'
import { apiJson } from '../../utils/apiFetch.js'
import { configFileName, configLines, configText, differences, importPlan, parseConfigText } from '../../utils/configBackup.js'
import { isSaveCancelled, isSaveCommandUnavailable, saveThroughDialog } from '../../utils/saveDialog.js'

// 配置文件: the files the host reads and writes, grouped by what they are for, each a row with its real place (the path that is set, or the default name in the host's
// configuration folder); a tap opens a sheet with the field, a file chooser where the product owns the file system (the control panel) and a copy button. Changing a path is
// confirmed once on save, which lists the changed paths. Then the log, the configuration as a file (export, import into the unsaved changes) and what differs from the defaults.
const props = defineProps(['platform', 'config'])
const { t, te } = useI18n()
// the default places (src/config.cpp: apps.json, sunshine_state.json and sunshine.log in the configuration folder, the certificate in its credentials folder)
const ROWS = {
  file_apps: { icon: 'apps', placeholder: 'apps.json', def: 'apps.json' },
  file_state: { icon: 'storage', placeholder: 'sunshine_state.json', def: 'sunshine_state.json' },
  // src/config.cpp:1460-1462: unset, the credentials are kept in the state file (whatever file_state is)
  credentials_file: { icon: 'key', placeholder: 'sunshine_state.json', def: 'sunshine_state.json', sameAsState: true },
  pkey: { icon: 'lock', placeholder: '/dir/pkey.pem', def: 'credentials/cakey.pem' },
  cert: { icon: 'verified_user', placeholder: '/dir/cert.pem', def: 'credentials/cacert.pem' },
  log_path: { icon: 'description', placeholder: 'sunshine.log', def: 'sunshine.log' },
}
const custom = (k) => !!props.config[k] && isChanged(k, props.config[k])
const defaultText = (k) => (ROWS[k].sameAsState ? t('config.path_same_as_state') : t('config.path_default_name', { name: ROWS[k].def }))
const valueOf = (k) => (props.config[k] ? String(props.config[k]) : defaultText(k))

const editing = ref('')
const sheet = computed({ get: () => !!editing.value, set: (v) => { if (!v) editing.value = '' } })
const row = computed(() => (editing.value ? { k: editing.value, ...ROWS[editing.value] } : null))
const text = computed({ get: () => props.config[editing.value] ?? '', set: (v) => { props.config[editing.value] = v } })
const browse = computed(() => (editing.value ? browsePath('file', () => t(`config.${editing.value}`)) : null))
const choose = async () => { const picked = await browse.value?.(); if (picked) text.value = picked }
const reset = () => { const d = SETTING_META[editing.value]?.def; text.value = d === null || d === undefined ? '' : (editing.value === 'pkey' || editing.value === 'cert' ? '' : d) }
const copied = ref(false)
const copyPath = async () => { try { await navigator.clipboard.writeText(String(text.value)); copied.value = true; setTimeout(() => { copied.value = false }, 1600) } catch { /* the path stays visible in the field */ } }

// the log: where it is written (a row of its own above), the level it records at (set on 常规 › 高级) and the page that shows it
const LOG_LEVELS = ['verbose', 'debug', 'info', 'warning', 'error', 'fatal', 'none']
const logLevel = computed(() => {
  const v = String(props.config.min_log_level ?? '2')
  const n = LOG_LEVELS.includes(v) ? LOG_LEVELS.indexOf(v) : Number.isInteger(Number(v)) ? Number(v) : 2
  return t(`config.log_level_${Math.min(Math.max(n, 0), 6)}`)
})
const go = (place) => { if (place) window.location.hash = settingsHash(place) }

// the configuration as a file: what the host has saved (GET /api/config), written as sunshine.conf; the control panel asks where with its save dialog, a browser downloads it
const exporting = ref(false)
const exportError = ref('')
async function exportConfig() {
  exporting.value = true
  exportError.value = ''
  try {
    const answer = await apiJson('/api/config')
    const host = answer.sunshine_name || ''
    const content = configText(configLines(answer), { host })
    const name = configFileName(host)
    const invoke = window.__TAURI_INTERNALS__?.invoke
    if (typeof invoke === 'function') {
      try {
        const saved = await saveThroughDialog(invoke, 'save_text_file', { content, defaultName: name, filterName: 'Sunshine', extensions: ['conf'] })
        if (saved === null) return
        return
      } catch (e) {
        if (isSaveCancelled(e)) return
        if (!isSaveCommandUnavailable(e)) throw e
      }
    }
    const url = URL.createObjectURL(new Blob([content], { type: 'text/plain' }))
    const link = Object.assign(document.createElement('a'), { href: url, download: name })
    document.body.appendChild(link); link.click(); link.remove()
    setTimeout(() => URL.revokeObjectURL(url), 10000)
  } catch (e) {
    exportError.value = String(e?.message || e)
  } finally { exporting.value = false }
}

// an import is read here and listed; what it changes goes into the unsaved changes of the page, and the page's save writes it
const picker = ref(null)
const plan = ref(null)
const planFile = ref('')
const planError = ref(false)
const importAsk = computed({ get: () => !!plan.value || planError.value, set: (v) => { if (!v) { plan.value = null; planError.value = false } } })
async function readImport(event) {
  const file = event.target.files?.[0]
  event.target.value = ''
  if (!file) return
  planFile.value = file.name
  const lines = parseConfigText(await file.text())
  if (!lines) { planError.value = true; return }
  plan.value = importPlan(lines, props.config)
}
const applyImport = () => { for (const c of plan.value?.changes || []) props.config[c.key] = c.to; plan.value = null }
const labelOf = (key, title) => (te(title) ? t(title) : te(`config.${key}`) ? t(`config.${key}`) : key)
const shown = (v, key = '') => {
  let s = String(v ?? '')
  if (s === '') return '—'
  if (s === 'enabled' || s === 'true') return t('_common.enabled')
  if (s === 'disabled' || s === 'false') return t('_common.disabled')
  if (key && te(`config.${key}_${s}`)) s = t(`config.${key}_${s}`)
  return s.length > 40 ? `${s.slice(0, 39)}…` : s
}

// what differs from the defaults, the way a profile is compared with the default one: the setting, its value and the default; a tap opens the setting where it lives
const diffs = computed(() => differences(props.config))
const allDiffs = ref(false)
const diffRows = computed(() => (allDiffs.value ? diffs.value : diffs.value.slice(0, 6)))
</script>

<template>
  <MSection :title="$t('config.files_section_state')">
    <MGroup>
      <MItem action icon="apps" data-cfg="file_apps" :title="$t('config.file_apps')" :text="valueOf('file_apps')" @click="editing = 'file_apps'">
        <template v-if="custom('file_apps')" #trail><MChip>{{ $t('config.path_custom') }}</MChip></template>
      </MItem>
      <MItem action icon="storage" data-cfg="file_state" :title="$t('config.file_state')" :text="valueOf('file_state')" @click="editing = 'file_state'">
        <template v-if="custom('file_state')" #trail><MChip>{{ $t('config.path_custom') }}</MChip></template>
      </MItem>
      <MItem action icon="key" data-cfg="credentials_file" :title="$t('config.credentials_file')" :text="valueOf('credentials_file')" @click="editing = 'credentials_file'">
        <template v-if="custom('credentials_file')" #trail><MChip>{{ $t('config.path_custom') }}</MChip></template>
      </MItem>
    </MGroup>
  </MSection>

  <MSection :title="$t('config.files_section_certs')">
    <MGroup>
      <MItem action icon="lock" data-cfg="pkey" :title="$t('config.pkey')" :text="valueOf('pkey')" @click="editing = 'pkey'">
        <template v-if="custom('pkey')" #trail><MChip>{{ $t('config.path_custom') }}</MChip></template>
      </MItem>
      <MItem action icon="verified_user" data-cfg="cert" :title="$t('config.cert')" :text="valueOf('cert')" @click="editing = 'cert'">
        <template v-if="custom('cert')" #trail><MChip>{{ $t('config.path_custom') }}</MChip></template>
      </MItem>
    </MGroup>
  </MSection>

  <MSection :title="$t('config.files_section_logs')">
    <MGroup>
      <MItem action icon="description" data-cfg="log_path" :title="$t('config.log_path')" :text="valueOf('log_path')" @click="editing = 'log_path'">
        <template v-if="custom('log_path')" #trail><MChip>{{ $t('config.path_custom') }}</MChip></template>
      </MItem>
      <MItem action icon="tune" :title="$t('config.log_level')" :text="logLevel" @click="go({ tab: 'general', sub: 'expert', focus: 'min_log_level' })" />
      <MItem href="/troubleshooting" icon="terminal" :title="$t('config.files_view_log')" />
    </MGroup>
  </MSection>

  <MSection :title="$t('config.files_section_backup')">
    <MGroup>
      <MItem action icon="download" :title="$t('config.config_export')" :text="exportError ? $t('config.config_export_failed', { error: exportError }) : $t('config.config_export_text')" :disabled="exporting" @click="exportConfig" />
      <MItem action icon="upload" :title="$t('config.config_import')" :text="$t('config.config_import_text')" @click="picker?.click()" />
    </MGroup>
    <input ref="picker" class="cfg-files__picker" type="file" accept=".conf,.txt,text/plain" tabindex="-1" aria-hidden="true" @change="readImport" />
  </MSection>

  <MSection :title="$t('config.files_section_diff')">
    <template v-if="diffs.length" #action><span class="cfg-files__count">{{ $t('config.files_diff_count', { n: diffs.length }) }}</span></template>
    <MGroup>
      <template v-if="diffs.length">
        <MItem v-for="d in diffRows" :key="d.key" :action="!!d.place" :data-diff="d.key" :title="labelOf(d.key, d.title)" :text="$t('config.files_diff_value', { value: shown(d.value, d.key), def: shown(d.def, d.key) })" @click="go(d.place)" />
        <MItem v-if="diffs.length > 6" action icon="more_horiz" :title="allDiffs ? $t('config.files_diff_less') : $t('config.files_diff_more', { n: diffs.length })" @click="allDiffs = !allDiffs" />
      </template>
      <MItem v-else icon="check_circle" :title="$t('config.files_diff_none')" />
    </MGroup>
  </MSection>

  <MSheet v-model="sheet" :title="row ? $t(`config.${row.k}`) : ''" side="center" :close-label="$t('_common.close')">
    <template v-if="row">
      <p v-if="$te(`config.${row.k}_desc`)" class="cfg-files__desc">{{ $t(`config.${row.k}_desc`) }}</p>
      <div class="cfg-files__field">
        <MField v-model="text" data-redact="path" :label="$t(`config.${row.k}`)" :placeholder="row.placeholder" :supporting="text ? '' : defaultText(row.k)" />
        <MButton v-if="browse" variant="tonal" icon="folder_open" @click="choose">{{ $t('_common.browse') }}</MButton>
        <MIconButton v-if="text" :icon="copied ? 'check' : 'content_copy'" :label="copied ? $t('_common.copied') : $t('config.path_copy')" @click="copyPath" />
      </div>
    </template>
    <template #actions>
      <MButton variant="text" icon="restart_alt" :disabled="!row || !isChanged(row.k, config[row.k])" @click="reset">{{ $t('config.expert_reset') }}</MButton>
      <MButton @click="sheet = false">{{ $t('_common.close') }}</MButton>
    </template>
  </MSheet>

  <MDialog v-model="importAsk" :title="$t('config.config_import_title', { name: planFile })" icon="upload" tone="info">
    <p v-if="planError">{{ $t('config.config_import_unreadable') }}</p>
    <template v-else-if="plan">
      <p>{{ plan.changes.length ? $t('config.config_import_changes', { n: plan.changes.length }) : $t('config.config_import_none') }}</p>
      <ul v-if="plan.changes.length" class="cfg-files__plan">
        <li v-for="c in plan.changes.slice(0, 12)" :key="c.key"><b>{{ labelOf(c.key, `config.${c.key}`) }}</b><span>{{ shown(c.from, c.key) }} → {{ shown(c.to, c.key) }}</span></li>
        <li v-if="plan.changes.length > 12" class="is-more">{{ $t('config.config_import_more', { n: plan.changes.length - 12 }) }}</li>
      </ul>
      <p v-if="plan.paths.length || plan.other.length" class="cfg-files__skip">
        <template v-if="plan.paths.length">{{ $t('config.config_import_paths', { n: plan.paths.length }) }}</template>
        <template v-if="plan.other.length"> {{ $t('config.config_import_other', { n: plan.other.length }) }}</template>
      </p>
    </template>
    <template #actions>
      <MButton variant="text" @click="importAsk = false">{{ $t('_common.cancel') }}</MButton>
      <MButton v-if="plan?.changes.length" @click="applyImport">{{ $t('config.config_import_apply') }}</MButton>
    </template>
  </MDialog>
</template>

<style>
.cfg-files__desc { margin: 0 0 12px; font: var(--m-body-medium); color: var(--md-sys-color-on-surface-variant); }
.cfg-files__field { display: flex; align-items: center; gap: 8px; } .cfg-files__field .m-field { flex: 1; min-width: 0; }
.cfg-files__picker { display: none; }
.cfg-files__count { font: var(--m-label-large); color: var(--md-sys-color-on-surface-variant); font-variant-numeric: tabular-nums; }
.cfg-files__plan { margin: 8px 0 0; padding: 0; list-style: none; display: flex; flex-direction: column; gap: 6px; max-height: 40vh; overflow: auto; font: var(--m-body-medium); }
.cfg-files__plan li { display: flex; flex-wrap: wrap; gap: 2px 12px; } .cfg-files__plan b { font: var(--m-label-large); } .cfg-files__plan span { color: var(--md-sys-color-on-surface-variant); overflow-wrap: anywhere; font-variant-numeric: tabular-nums; }
.cfg-files__plan li.is-more, .cfg-files__skip { color: var(--md-sys-color-on-surface-variant); font: var(--m-body-small); }
</style>
