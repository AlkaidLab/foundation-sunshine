<template>
  <MSheet :model-value="show" :title="$t('troubleshooting.ai_diagnosis_title')" side="end" :close-label="$t('_common.close')" @update:model-value="onSheet">
    <MSection>
      <MGroup>
        <MItem action icon="tune" :title="$t('troubleshooting.ai_config')" :aria-expanded="showConfig" @click="showConfig = !showConfig">
          <template #trail><MLoading v-if="isConfigLoading || isSavingConfig" size="20px" :label="loadingStatusText" /><MIcon :name="showConfig ? 'keyboard_arrow_up' : 'keyboard_arrow_down'" /></template>
        </MItem>
        <template v-if="showConfig">
          <MItem :title="diagnosisText('status')" :value="config?.enabled ? diagnosisText('enabled') : diagnosisText('disabled')" />
          <MItem :title="$t('troubleshooting.ai_provider')" :value="config?.provider || '-'" />
          <MItem :title="diagnosisText('compatibility')" :value="config?.compatibility || '-'" />
          <MItem :title="$t('troubleshooting.ai_model')" :value="config?.model || '-'" />
        </template>
      </MGroup>
      <MFooter v-if="showConfig">{{ diagnosisText('managedConfig') }} {{ diagnosisText('keysLocal') }}</MFooter>
    </MSection>

    <MSection v-if="localFindings?.length" :title="diagnosisText('localPreDiagnosis')">
      <MGroup>
        <MCheckItem v-for="finding in localFindings" :key="finding.id" :state="checkState(finding.severity)" :title="getFindingLabel(finding)" :text="finding.count > 1 ? `×${finding.count}` : ''">
          <template v-if="finding.evidence?.[0]?.text" #default><code class="m-tip">{{ finding.evidence[0].text }}</code></template>
        </MCheckItem>
      </MGroup>
    </MSection>

    <MSection v-if="localSuggestions?.length" :title="diagnosisText('suggestedFixes')">
      <MGroup>
        <MCheckItem v-for="suggestion in localSuggestions" :key="suggestion.id" :state="checkState(suggestion.severity)" :title="getSuggestionTitle(suggestion)">
          <ul class="m-tip">
            <li v-for="(action, index) in getSuggestionActions(suggestion)" :key="`${index}-${action}`">{{ action }}</li>
          </ul>
        </MCheckItem>
      </MGroup>
    </MSection>

    <div v-if="isLoading && !result" class="diag-wait"><MLoading /><span>{{ $t('troubleshooting.ai_analyzing_logs') }}</span></div>

    <MBanner v-if="error" tone="error" icon="error">
      <b>{{ $t('troubleshooting.ai_error') }}</b> {{ error }}
    </MBanner>

    <MSection v-if="result" :title="$t('troubleshooting.ai_result')">
      <div class="m-tip diag-result">
        <MIconButton class="diag-copy" icon="content_copy" :label="$t('troubleshooting.ai_copy_result')" @click="copyResult" />
        <div class="result-content" v-html="renderMarkdown(result)"></div>
      </div>
    </MSection>

    <template #actions>
      <MButton v-if="!result && !error" size="lg" :loading="isLoading" :disabled="isLoading" icon="wand_stars" @click="$emit('diagnose')">{{ isLoading ? $t('troubleshooting.ai_analyzing') : $t('troubleshooting.ai_start_diagnosis') }}</MButton>
      <MButton v-else variant="tonal" size="lg" :loading="isLoading" :disabled="isLoading" icon="refresh" @click="$emit('diagnose')">{{ error ? $t('troubleshooting.ai_retry') : $t('troubleshooting.ai_reanalyze') }}</MButton>
    </template>
  </MSheet>
</template>

<script setup>
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { MBanner, MButton, MCheckItem, MFooter, MGroup, MIcon, MIconButton, MItem, MLoading, MSection, MSheet } from 'foundation-ui/vue'

const { locale } = useI18n()

const props = defineProps({
  show: Boolean,
  config: Object,
  isConfigLoading: Boolean,
  isSavingConfig: Boolean,
  isLoading: Boolean,
  result: String,
  error: String,
  localFindings: {
    type: Array,
    default: () => [],
  },
  localSuggestions: {
    type: Array,
    default: () => [],
  },
})

const emit = defineEmits(['close', 'diagnose'])
const checkState = (severity) => (severity === 'fatal' || severity === 'error' ? 'error' : severity === 'warning' ? 'warn' : 'info')
const onSheet = (open) => { if (!open) emit('close') }

const showConfig = ref(false)

const DIAGNOSIS_TEXT = {
  en: {
    managedConfig: 'AI configuration is managed in the Mita AI Assistant control panel. This dialog only uses the shared Sunshine AI proxy.',
    status: 'Status',
    enabled: 'Enabled',
    disabled: 'Disabled',
    compatibility: 'Compatibility',
    keysLocal: "API keys stay in Sunshine's local config and are not edited here.",
    localPreDiagnosis: 'Local pre-diagnosis',
    suggestedFixes: 'Suggested fixes',
    loadingConfig: 'Loading AI configuration...',
    savingConfig: 'Saving AI configuration...',
  },
  zh: {
    managedConfig: 'AI \u914d\u7f6e\u7531\u7c73\u5854 AI \u52a9\u624b\u63a7\u5236\u9762\u677f\u7edf\u4e00\u7ba1\u7406\u3002\u6b64\u5f39\u7a97\u53ea\u4f7f\u7528 Sunshine \u5171\u4eab AI \u4ee3\u7406\u3002',
    status: '\u72b6\u6001',
    enabled: '\u5df2\u542f\u7528',
    disabled: '\u5df2\u7981\u7528',
    compatibility: '\u517c\u5bb9\u6a21\u5f0f',
    keysLocal: 'API \u5bc6\u94a5\u4fdd\u5b58\u5728 Sunshine \u672c\u5730\u914d\u7f6e\u4e2d\uff0c\u6b64\u5904\u4e0d\u4f1a\u7f16\u8f91\u3002',
    localPreDiagnosis: '\u672c\u5730\u9884\u8bca\u65ad',
    suggestedFixes: '\u5efa\u8bae\u4fee\u590d',
    loadingConfig: '\u6b63\u5728\u52a0\u8f7d AI \u914d\u7f6e...',
    savingConfig: '\u6b63\u5728\u4fdd\u5b58 AI \u914d\u7f6e...',
  },
}

function getCurrentLocale() {
  const documentLocale = typeof document !== 'undefined'
    ? document.documentElement?.getAttribute?.('lang')
    : ''
  return String(locale.value || documentLocale || '').toLowerCase()
}

function diagnosisText(key) {
  const bucket = getCurrentLocale().startsWith('zh') ? DIAGNOSIS_TEXT.zh : DIAGNOSIS_TEXT.en
  return bucket[key] || DIAGNOSIS_TEXT.en[key] || key
}

const loadingStatusText = computed(() => (
  props.isSavingConfig ? diagnosisText('savingConfig') : diagnosisText('loadingConfig')
))

function copyResult() {
  const el = document.querySelector('.result-content')
  if (el) {
    navigator.clipboard.writeText(el.innerText)
  }
}

function getFindingLabel(finding) {
  if (getCurrentLocale().startsWith('zh') && finding?.labels?.zh) {
    return finding.labels.zh
  }
  return finding?.message || finding?.type || ''
}

function getSuggestionTitle(suggestion) {
  if (getCurrentLocale().startsWith('zh') && suggestion?.labels?.zh) {
    return suggestion.labels.zh
  }
  return suggestion?.title || suggestion?.findingType || ''
}

function getSuggestionActions(suggestion) {
  if (getCurrentLocale().startsWith('zh') && Array.isArray(suggestion?.actionLabels?.zh)) {
    return suggestion.actionLabels.zh
  }
  return Array.isArray(suggestion?.actions) ? suggestion.actions : []
}

function renderMarkdown(text) {
  if (!text) return ''
  return text
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/```([\s\S]*?)```/g, '<pre class="code-block">$1</pre>')
    .replace(/`([^`]+)`/g, '<code>$1</code>')
    .replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>')
    .replace(/^\s*[-*]\s+(.+)$/gm, '<li>$1</li>')
    .replace(/(<li>.*<\/li>)/s, '<ul>$1</ul>')
    .replace(/^### (.+)$/gm, '<h6>$1</h6>')
    .replace(/^## (.+)$/gm, '<h5>$1</h5>')
    .replace(/^# (.+)$/gm, '<h4>$1</h4>')
    .replace(/\n/g, '<br>')
}
</script>

<style>
.diag-wait { display: flex; align-items: center; gap: 12px; padding: 16px; color: var(--md-sys-color-on-surface-variant); }
.diag-result { position: relative; padding-inline-end: 56px; }
.diag-copy { position: absolute; top: 8px; right: 8px; }
.diag-result .result-content { font: var(--m-body-medium); line-height: 1.6; word-break: break-word; }
.diag-result h4, .diag-result h5, .diag-result h6 { margin: 12px 0 4px; }
.diag-result .code-block { margin: 8px 0; padding: 12px; border-radius: 12px; background: var(--md-sys-color-surface-container-highest); overflow: auto; white-space: pre-wrap; }
</style>
