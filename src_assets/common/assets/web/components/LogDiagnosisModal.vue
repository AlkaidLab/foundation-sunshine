<template>
  <Transition name="fade">
    <div v-if="show" class="diagnosis-overlay" @click.self="$emit('close')">
      <div class="diagnosis-modal">
        <div class="diagnosis-header">
          <h5>
            <i class="fas fa-robot me-2"></i>{{ $t('troubleshooting.ai_diagnosis_title') }}
          </h5>
          <button class="btn-close" :aria-label="$t('close')" @click="$emit('close')"></button>
        </div>

        <div class="diagnosis-body">
          <!-- Config Section (collapsible) -->
          <div class="config-section mb-3">
            <button
              class="btn btn-sm btn-outline-secondary w-100 text-start"
              @click="showConfig = !showConfig"
            >
              <i class="fas fa-cog me-1"></i>
              {{ $t('troubleshooting.ai_config') }}
              <i class="fas ms-1" :class="showConfig ? 'fa-chevron-up' : 'fa-chevron-down'"></i>
            </button>

            <div v-if="showConfig" class="config-form mt-2">
              <div class="row g-2">
                <div class="col-md-4">
                  <label class="form-label form-label-sm">{{ $t('troubleshooting.ai_provider') }}</label>
                  <select class="form-select form-select-sm" v-model="config.provider" @change="onProviderChange(config.provider)">
                    <option v-for="p in providers" :key="p.value" :value="p.value">{{ p.label }}</option>
                  </select>
                </div>
                <div class="col-md-4">
                  <label class="form-label form-label-sm">API Key</label>
                  <input type="password" class="form-control form-control-sm" v-model="config.apiKey" placeholder="sk-..." />
                </div>
                <div class="col-md-4">
                  <label class="form-label form-label-sm">{{ $t('troubleshooting.ai_model') }}</label>
                  <input type="text" class="form-control form-control-sm" v-model="config.model" :placeholder="getAvailableModels()[0] || 'model'" list="ai-models" />
                  <datalist id="ai-models">
                    <option v-for="m in getAvailableModels()" :key="m" :value="m" />
                  </datalist>
                </div>
              </div>
              <div class="row g-2 mt-1" v-if="config.provider === 'custom'">
                <div class="col-12">
                  <label class="form-label form-label-sm">API Base</label>
                  <input type="text" class="form-control form-control-sm" v-model="config.apiBase" placeholder="https://api.example.com/v1" />
                </div>
              </div>
              <div class="text-muted small mt-1">
                <i class="fas fa-lock me-1"></i>{{ $t('troubleshooting.ai_key_local') }}
              </div>
            </div>
          </div>

          <!-- Diagnose Button -->
          <div class="text-center mb-3" v-if="!result && !error">
            <button class="btn btn-primary" :disabled="isLoading" @click="$emit('diagnose')">
              <span v-if="isLoading">
                <span class="spinner-border spinner-border-sm me-1"></span>
                {{ $t('troubleshooting.ai_analyzing') }}
              </span>
              <span v-else>
                <i class="fas fa-search-plus me-1"></i>
                {{ $t('troubleshooting.ai_start_diagnosis') }}
              </span>
            </button>
          </div>

          <!-- Loading -->
          <div v-if="isLoading && !result" class="text-center text-muted py-4">
            <div class="spinner-border text-primary mb-2"></div>
            <p>{{ $t('troubleshooting.ai_analyzing_logs') }}</p>
          </div>

          <!-- Error -->
          <div v-if="error" class="alert alert-danger d-flex align-items-start">
            <i class="fas fa-exclamation-circle me-2 mt-1"></i>
            <div>
              <strong>{{ $t('troubleshooting.ai_error') }}</strong>
              <p class="mb-1">{{ error }}</p>
              <button class="btn btn-sm btn-outline-danger" @click="$emit('diagnose')">
                <i class="fas fa-redo me-1"></i>{{ $t('troubleshooting.ai_retry') }}
              </button>
            </div>
          </div>

          <!-- Result -->
          <div v-if="result" class="diagnosis-result">
            <div class="result-header mb-2">
              <i class="fas fa-clipboard-check text-success me-1"></i>
              <strong>{{ $t('troubleshooting.ai_result') }}</strong>
              <button class="btn btn-sm btn-outline-secondary ms-auto" @click="copyResult">
                <i class="fas fa-copy me-1"></i>{{ $t('troubleshooting.ai_copy_result') }}
              </button>
            </div>
            <div class="result-content" v-html="renderMarkdown(result)"></div>
            <div class="text-center mt-3">
              <button class="btn btn-sm btn-outline-primary" @click="$emit('diagnose')">
                <i class="fas fa-redo me-1"></i>{{ $t('troubleshooting.ai_reanalyze') }}
              </button>
            </div>
          </div>
        </div>
      </div>
    </div>
  </Transition>
</template>

<script setup>
import { ref } from 'vue'
import { useI18n } from 'vue-i18n'

const { t } = useI18n()

defineProps({
  show: Boolean,
  config: Object,
  providers: Array,
  isLoading: Boolean,
  result: String,
  error: String,
  onProviderChange: Function,
  getAvailableModels: Function,
})

defineEmits(['close', 'diagnose'])

const showConfig = ref(false)

function copyResult() {
  const el = document.querySelector('.result-content')
  if (el) {
    navigator.clipboard.writeText(el.innerText)
  }
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
    .replace(/^### (.+)$/gm, '<h6 class="mt-3 mb-1">$1</h6>')
    .replace(/^## (.+)$/gm, '<h5 class="mt-3 mb-1">$1</h5>')
    .replace(/^# (.+)$/gm, '<h4 class="mt-3 mb-1">$1</h4>')
    .replace(/\n/g, '<br>')
}
</script>

<style scoped>
.diagnosis-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background: rgba(0, 0, 0, 0.5);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1050;
  backdrop-filter: blur(4px);
}

.diagnosis-modal {
  background: var(--bs-body-bg, #fff);
  border-radius: 12px;
  width: 90%;
  max-width: 700px;
  max-height: 80vh;
  display: flex;
  flex-direction: column;
  box-shadow: 0 20px 60px rgba(0, 0, 0, 0.3);
}

.diagnosis-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 1rem 1.5rem;
  border-bottom: 1px solid var(--bs-border-color, #dee2e6);
}

.diagnosis-header h5 {
  margin: 0;
  font-weight: 600;
}

.diagnosis-body {
  padding: 1.5rem;
  overflow-y: auto;
  flex: 1;
}

.config-form {
  background: var(--bs-tertiary-bg, #f8f9fa);
  border-radius: 8px;
  padding: 1rem;
}

.result-header {
  display: flex;
  align-items: center;
}

.result-content {
  background: var(--bs-tertiary-bg, #f8f9fa);
  border-radius: 8px;
  padding: 1rem 1.25rem;
  font-size: 0.9rem;
  line-height: 1.6;
  max-height: 400px;
  overflow-y: auto;
}

.result-content :deep(code) {
  background: rgba(0, 0, 0, 0.08);
  padding: 0.15em 0.4em;
  border-radius: 3px;
  font-size: 0.85em;
}

.result-content :deep(.code-block) {
  background: #1e1e1e;
  color: #d4d4d4;
  padding: 0.75rem 1rem;
  border-radius: 6px;
  font-size: 0.8rem;
  overflow-x: auto;
}

.result-content :deep(ul) {
  padding-left: 1.5rem;
  margin: 0.5rem 0;
}

.result-content :deep(li) {
  margin-bottom: 0.25rem;
}

.fade-enter-active,
.fade-leave-active {
  transition: opacity 0.2s ease;
}

.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}
</style>
