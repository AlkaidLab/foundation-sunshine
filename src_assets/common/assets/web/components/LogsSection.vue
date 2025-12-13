<template>
  <div class="card shadow-sm mb-4">
    <div class="card-header bg-dark bg-opacity-10 border-bottom-0">
      <div class="d-flex justify-content-between align-items-center flex-wrap gap-2">
        <h5 class="card-title mb-0">
          <i class="fas fa-file-alt text-dark me-2"></i>
          {{ $t('troubleshooting.logs') }}
        </h5>
        <div class="d-flex align-items-center gap-2">
          <div class="input-group" style="width: 480px">
            <span class="input-group-text">
              <i class="fas fa-search text-muted"></i>
            </span>
            <input
              type="text"
              class="form-control"
              v-model="logFilterModel"
              :placeholder="$t('troubleshooting.logs_find')"
            />
            <input type="checkbox" class="btn-check" id="ignoreCase" v-model="ignoreCaseModel" />
            <label
              class="btn btn-outline-secondary match-mode-btn"
              for="ignoreCase"
              :title="$t('troubleshooting.ignore_case')"
            >
              <i class="fas fa-font"></i>
            </label>
            <template v-for="mode in matchModes" :key="mode.value">
              <input
                type="radio"
                class="btn-check"
                name="matchMode"
                :id="`matchMode${mode.value}`"
                :value="mode.value"
                v-model="matchModeModel"
              />
              <label
                class="btn btn-outline-secondary match-mode-btn"
                :for="`matchMode${mode.value}`"
                :title="$t(mode.labelKey)"
              >
                <i :class="mode.icon"></i>
              </label>
            </template>
          </div>
          <button class="btn btn-outline-success" @click="downloadLogs">
            <i class="fas fa-download me-1"></i>
            {{ $t('troubleshooting.download_logs') }}
          </button>
          <button class="btn btn-outline-primary" @click="copyConfig">
            <i class="fas fa-copy me-1"></i>
            {{ $t('troubleshooting.copy_config') }}
          </button>
        </div>
      </div>
    </div>
    <div class="card-body">
      <p class="text-muted mb-3">{{ $t('troubleshooting.logs_desc') }}</p>
      <div class="logs-container">
        <button class="copy-btn" @click="copyLogs" :title="$t('troubleshooting.copy_logs')">
          <i class="fas fa-copy"></i>
        </button>
        <pre class="logs-content">{{ actualLogs }}</pre>
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed } from 'vue'

const props = defineProps({
  logFilter: {
    type: String,
    default: null,
  },
  actualLogs: {
    type: String,
    required: true,
  },
  copyLogs: {
    type: Function,
    required: true,
  },
  copyConfig: {
    type: Function,
    required: true,
  },
  matchMode: {
    type: String,
    default: 'contains',
  },
  ignoreCase: {
    type: Boolean,
    default: true,
  },
})

const emit = defineEmits(['update:logFilter', 'update:matchMode', 'update:ignoreCase'])

const matchModes = [
  { value: 'contains', labelKey: 'troubleshooting.match_contains', icon: 'fas fa-filter' },
  { value: 'regex', labelKey: 'troubleshooting.match_regex', icon: 'fas fa-code' },
  { value: 'exact', labelKey: 'troubleshooting.match_exact', icon: 'fas fa-equals' },
]

const logFilterModel = computed({
  get: () => props.logFilter ?? '',
  set: (value) => emit('update:logFilter', value || null),
})

const matchModeModel = computed({
  get: () => props.matchMode,
  set: (value) => emit('update:matchMode', value),
})

const ignoreCaseModel = computed({
  get: () => props.ignoreCase,
  set: (value) => emit('update:ignoreCase', value),
})

const downloadLogs = () => {
  const timestamp = new Date().toISOString().slice(0, 19).replace(/[:.]/g, '-')
  const blob = new Blob([props.actualLogs], { type: 'text/plain;charset=utf-8' })
  const url = URL.createObjectURL(blob)
  
  const link = Object.assign(document.createElement('a'), {
    href: url,
    download: `sunshine-logs-${timestamp}.txt`,
  })
  
  link.click()
  URL.revokeObjectURL(url)
}
</script>

<style scoped>
.input-group .btn {
  border-radius: 0;
  padding: 0.375rem 0.5rem;
  font-size: 0.875rem;
}

.input-group .btn:last-of-type {
  border-radius: 0 0.375rem 0.375rem 0;
}

.input-group .btn:hover {
  transform: none;
}

.match-mode-btn {
  min-width: 36px;
  padding: 0.375rem 0.75rem !important;
  display: flex;
  align-items: center;
  justify-content: center;
  transition: all 0.2s ease;
}

.match-mode-btn i {
  font-size: 0.875rem;
  line-height: 1;
}

.match-mode-btn:hover {
  background-color: rgba(108, 117, 125, 0.1);
  border-color: #6c757d;
}

.btn-check:checked + .match-mode-btn {
  background-color: #6c757d;
  border-color: #6c757d;
  color: #fff;
}

.btn-check:checked + .match-mode-btn:hover {
  background-color: #5a6268;
  border-color: #545b62;
}

.logs-container {
  position: relative;
  background: #1e1e1e;
  border-radius: 10px;
  overflow: hidden;
}

.logs-content {
  margin: 0;
  padding: 1.25rem;
  font-family: 'Consolas', 'Monaco', 'Courier New', monospace;
  font-size: 0.85rem;
  line-height: 1.5;
  color: #d4d4d4;
  overflow: auto;
  max-height: 450px;
  min-height: 300px;
  white-space: pre-wrap;
  word-break: break-all;
  scrollbar-width: thin;
  scrollbar-color: #666 #1e1e1e;
}

.logs-content::-webkit-scrollbar {
  width: 8px;
  height: 8px;
}

.logs-content::-webkit-scrollbar-track {
  background: #1e1e1e;
  border-radius: 4px;
}

.logs-content::-webkit-scrollbar-thumb {
  background: #666;
  border-radius: 4px;
}

.logs-content::-webkit-scrollbar-thumb:hover {
  background: #888;
}

.copy-btn {
  position: absolute;
  top: 12px;
  right: 12px;
  padding: 8px 12px;
  cursor: pointer;
  color: #ffffff;
  background: rgba(255, 255, 255, 0.1);
  border: none;
  border-radius: 6px;
  transition: all 0.2s ease;
  z-index: 10;
}

.copy-btn:hover {
  background: rgba(255, 255, 255, 0.2);
  transform: scale(1.05);
}

.copy-btn:active {
  transform: scale(0.95);
}

.input-group-text {
  border-right: none;
  background-color: #fff;
  
  [data-bs-theme='dark'] & {
    background-color: #212529;
    color: #fff;
  }
}

.input-group .form-control {
  border-left: none;
  border-right: none;
}

.input-group .form-control:focus {
  border-color: #ced4da;
  box-shadow: none;
}

.input-group:focus-within {
  box-shadow: 0 0 0 0.25rem rgba(13, 110, 253, 0.25);
  border-radius: 0.375rem;
}

.input-group:focus-within .input-group-text,
.input-group:focus-within .form-control {
  border-color: #86b7fe;
}

@media (max-width: 991.98px) {
  .card-header .d-flex {
    flex-direction: column;
    align-items: flex-start !important;
  }

  .card-header .input-group {
    width: 100% !important;
    margin-top: 0.5rem;
  }
}
</style>
