<template>
  <div class="setup-container">
    <div class="setup-card">
      <div class="setup-header">
        <img src="/images/logo-sunshine-45.png" height="60" alt="Sunshine">
        <h1>{{ $t('setup.welcome') }}</h1>
        <p>{{ $t('setup.description') }}</p>
      </div>

      <div class="setup-content">
        <!-- 步骤指示器 -->
        <div class="step-indicator">
          <div class="step" :class="{ active: currentStep === 1, completed: currentStep > 1 }">
            <div class="step-number">1</div>
            <span>{{ $t('setup.step0_title') }}</span>
          </div>
          <div class="step-connector"></div>
          <div class="step" :class="{ active: currentStep === 2, completed: currentStep > 2 }">
            <div class="step-number">2</div>
            <span>{{ $t('setup.step1_title') }}</span>
          </div>
          <div class="step-connector"></div>
          <div class="step" :class="{ active: currentStep === 3, completed: currentStep > 3 }">
            <div class="step-number">3</div>
            <span>{{ $t('setup.step2_title') }}</span>
          </div>
          <div class="step-connector"></div>
          <div class="step" :class="{ active: currentStep === 4, completed: currentStep > 4 }">
            <div class="step-number">4</div>
            <span>{{ $t('setup.step3_title') }}</span>
          </div>
          <div class="step-connector"></div>
          <div class="step" :class="{ active: currentStep === 5 }">
            <div class="step-number">5</div>
            <span>{{ $t('setup.step4_title') }}</span>
          </div>
        </div>

        <!-- 步骤内容 -->
        <div class="step-content">
          <!-- 步骤 1: 选择语言 -->
          <div v-if="currentStep === 1">
            <h3 class="mb-4">{{ $t('setup.step0_description') }}</h3>
            
            <div class="option-card" 
                 :class="{ selected: selectedLocale === 'zh' }"
                 @click="selectedLocale = 'zh'">
              <div class="option-icon">
                <i class="fas fa-language"></i>
              </div>
              <h4>简体中文</h4>
              <p>使用简体中文界面</p>
            </div>

            <div class="option-card" 
                 :class="{ selected: selectedLocale === 'en' }"
                 @click="selectedLocale = 'en'">
              <div class="option-icon">
                <i class="fas fa-language"></i>
              </div>
              <h4>English</h4>
              <p>Use English interface</p>
            </div>
          </div>

          <!-- 步骤 2: 选择串流方式 -->
          <div v-else-if="currentStep === 2">
            <h3 class="mb-4">{{ $t('setup.step1_description') }}</h3>
            
            <div class="option-card" 
                 :class="{ selected: streamMode === 'physical' }"
                 @click="streamMode = 'physical'">
              <div class="option-icon">
                <i class="fas fa-desktop"></i>
              </div>
              <h4>{{ $t('setup.physical_display') }}</h4>
              <p>{{ $t('setup.physical_display_desc') }}</p>
            </div>

            <div class="option-card" 
                 :class="{ selected: streamMode === 'virtual' }"
                 @click="streamMode = 'virtual'">
              <div class="option-icon">
                <i class="fas fa-tv"></i>
              </div>
              <h4>{{ $t('setup.virtual_display') }}</h4>
              <p>{{ $t('setup.virtual_display_desc') }}</p>
            </div>
          </div>

          <!-- 步骤 3: 选择显卡 -->
          <div v-else-if="currentStep === 3">
            <h3 class="mb-4">{{ $t('setup.step2_description') }}</h3>
            
            <div class="mb-3">
              <label for="adapterSelect" class="form-label">{{ $t('setup.select_adapter') }}</label>
              <select id="adapterSelect" 
                      class="form-select form-select-large" 
                      v-model="selectedAdapter">
                <option value="">{{ $t('setup.choose_adapter') }}</option>
                <option v-for="adapter in adapters" :key="adapter.name" :value="adapter.name">
                  {{ adapter.name }}
                </option>
              </select>
            </div>

            <div v-if="selectedAdapter" class="adapter-info">
              <h5>
                <i class="fas fa-info-circle"></i>
                {{ $t('setup.adapter_info') }}
              </h5>
              <p><strong>{{ $t('setup.selected_adapter') }}:</strong> {{ selectedAdapter }}</p>
              <p><strong>{{ $t('setup.stream_mode') }}:</strong> 
                {{ streamMode === 'physical' ? $t('setup.physical_display') : $t('setup.virtual_display') }}
              </p>
            </div>
          </div>

          <!-- 步骤 4: 选择显示器组合策略 -->
          <div v-else-if="currentStep === 4">
            <h3 class="mb-4">{{ $t('setup.step3_description') }}</h3>
            
            <div class="option-card" 
                 :class="{ selected: displayDevicePrep === 'ensure_only_display' }"
                 @click="displayDevicePrep = 'ensure_only_display'">
              <div class="option-icon">
                <i class="fas fa-desktop"></i>
              </div>
              <h4>{{ $t('setup.ensure_only_display') }}</h4>
              <p>{{ $t('setup.ensure_only_display_desc') }}</p>
            </div>

            <div class="option-card" 
                 :class="{ selected: displayDevicePrep === 'ensure_primary' }"
                 @click="displayDevicePrep = 'ensure_primary'">
              <div class="option-icon">
                <i class="fas fa-star"></i>
              </div>
              <h4>{{ $t('setup.ensure_primary') }}</h4>
              <p>{{ $t('setup.ensure_primary_desc') }}</p>
            </div>

            <div class="option-card" 
                 :class="{ selected: displayDevicePrep === 'ensure_active' }"
                 @click="displayDevicePrep = 'ensure_active'">
              <div class="option-icon">
                <i class="fas fa-check-circle"></i>
              </div>
              <h4>{{ $t('setup.ensure_active') }}</h4>
              <p>{{ $t('setup.ensure_active_desc') }}</p>
            </div>

            <div class="option-card" 
                 :class="{ selected: displayDevicePrep === 'no_operation' }"
                 @click="displayDevicePrep = 'no_operation'">
              <div class="option-icon">
                <i class="fas fa-hand-paper"></i>
              </div>
              <h4>{{ $t('setup.no_operation') }}</h4>
              <p>{{ $t('setup.no_operation_desc') }}</p>
            </div>
          </div>

          <!-- 步骤 5: 完成 -->
          <div v-else-if="currentStep === 5">
            <div class="text-center">
              <div class="mb-4" style="font-size: 4em; color: #28a745;">
                <i class="fas fa-check-circle"></i>
              </div>
              <h3 class="mb-3">{{ $t('setup.setup_complete') }}</h3>
              <p class="mb-4">{{ $t('setup.setup_complete_desc') }}</p>
              
              <div class="alert alert-info" v-if="saveSuccess">
                <i class="fas fa-info-circle"></i>
                {{ $t('setup.config_saved') }}
              </div>
              
              <div class="alert alert-danger" v-if="saveError">
                <i class="fas fa-exclamation-triangle"></i>
                {{ saveError }}
              </div>
            </div>
          </div>
        </div>

        <!-- 操作按钮 -->
        <div class="action-buttons">
          <button class="btn btn-setup btn-setup-secondary" 
                  @click="previousStep" 
                  v-if="currentStep > 1 && currentStep < 5"
                  :disabled="saving">
            <i class="fas fa-arrow-left"></i>
            {{ $t('setup.previous') }}
          </button>
          <div v-else></div>

          <button class="btn btn-setup btn-setup-primary" 
                  @click="nextStep" 
                  v-if="currentStep < 5"
                  :disabled="!canProceed || saving">
            {{ currentStep === 4 ? $t('setup.finish') : $t('setup.next') }}
            <i class="fas fa-arrow-right"></i>
          </button>

          <button class="btn btn-setup btn-setup-primary" 
                  @click="goToApps" 
                  v-if="currentStep === 5">
            {{ $t('setup.go_to_apps') }}
            <i class="fas fa-arrow-right"></i>
          </button>
        </div>
      </div>
    </div>
  </div>
</template>

<script>
import { trackEvents } from './firebase-config'

export default {
  name: 'SetupWizard',
  props: {
    adapters: {
      type: Array,
      default: () => []
    },
    displayDevices: {
      type: Array,
      default: () => []
    },
    hasLocale: {
      type: Boolean,
      default: false
    }
  },
  data() {
    return {
      currentStep: 1,
      selectedLocale: 'zh', // 默认中文
      streamMode: null,
      selectedAdapter: '',
      displayDevicePrep: 'ensure_only_display', // 默认选择：确保唯一显示器
      saveSuccess: false,
      saveError: null,
      saving: false,
    }
  },
  mounted() {
    // 记录进入设置向导
    trackEvents.pageView('setup_wizard')
    trackEvents.userAction('setup_wizard_started', {
      has_locale: this.hasLocale,
      adapter_count: this.adapters.length
    })
    
    // 如果已经有语言配置，跳过第一步
    if (this.hasLocale) {
      this.currentStep = 2
      trackEvents.userAction('setup_wizard_skip_language', { 
        reason: 'already_configured' 
      })
    }
    
    // 如果只有一个显卡，自动选择
    if (this.adapters.length === 1) {
      this.selectedAdapter = this.adapters[0].name
    }
  },
  computed: {
    canProceed() {
      if (this.currentStep === 1) {
        return this.selectedLocale !== null
      } else if (this.currentStep === 2) {
        return this.streamMode !== null
      } else if (this.currentStep === 3) {
        return this.selectedAdapter !== ''
      } else if (this.currentStep === 4) {
        return this.displayDevicePrep !== null
      }
      return false
    }
  },
  methods: {
    previousStep() {
      if (this.currentStep > 1) {
        this.currentStep--
      }
    },
    async nextStep() {
      if (this.currentStep === 1 && this.canProceed) {
        // 保存语言设置并刷新
        await this.saveLanguage()
      } else if (this.currentStep === 2 && this.canProceed) {
        this.currentStep++
      } else if (this.currentStep === 3 && this.canProceed) {
        this.currentStep++
      } else if (this.currentStep === 4 && this.canProceed) {
        await this.saveConfiguration()
      }
    },
    async saveLanguage() {
      try {
        await fetch('/api/config', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json',
          },
          body: JSON.stringify({
            locale: this.selectedLocale
          }),
        })
        // 重新加载页面以应用新语言
        window.location.reload()
      } catch (error) {
        console.error('Failed to save language:', error)
      }
    },
    async saveConfiguration() {
      this.saving = true
      this.saveError = null

      try {
        // 先获取当前配置，保留已有的设置（如 locale）
        const currentConfig = await fetch('/api/config').then(r => r.json())
        
        const config = {
          adapter_name: this.selectedAdapter,
        }

        // 保留已有的 locale 设置
        if (currentConfig.locale) {
          config.locale = currentConfig.locale
        }

        // 如果选择虚拟显示器，添加虚拟显示器相关配置
        if (this.streamMode === 'virtual') {
          config.output_name = 'ZakoHDR'
        }

        // 添加显示器组合策略
        config.display_device_prep = this.displayDevicePrep

        console.log('保存配置（包含 locale 和 display_device_prep）:', config)

        const response = await fetch('/api/config', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json',
          },
          body: JSON.stringify(config),
        })

        if (response.ok) {
          this.saveSuccess = true
          this.currentStep = 5
          
          // 记录设置完成
          trackEvents.userAction('setup_wizard_completed', {
            stream_mode: this.streamMode,
            adapter: this.selectedAdapter,
            display_device_prep: this.displayDevicePrep,
            has_virtual_display: this.streamMode === 'virtual'
          })
          
          this.$emit('setup-complete', config)
        } else {
          const errorText = await response.text()
          this.saveError = `${this.$t('setup.save_error')}: ${errorText}`
          
          // 记录保存失败
          trackEvents.errorOccurred('setup_config_save_failed', errorText)
        }
      } catch (error) {
        console.error('Failed to save configuration:', error)
        this.saveError = `${this.$t('setup.save_error')}: ${error.message}`
      } finally {
        this.saving = false
      }
    },
    goToApps() {
      // 记录跳转到应用配置页面
      trackEvents.userAction('setup_go_to_apps', {
        from_step: this.currentStep
      })
      window.location.href = '/apps'
    }
  }
}
</script>

<style scoped>
.setup-container {
  max-width: 900px;
  margin: 1em auto;
  padding: 0 1em;
  height: calc(100vh - 2em);
  display: flex;
  flex-direction: column;
}

.setup-card {
  background: var(--bs-body-bg);
  border-radius: 16px;
  box-shadow: 0 4px 20px rgba(0, 0, 0, 0.1);
  overflow: hidden;
  display: flex;
  flex-direction: column;
  height: 100%;
}

.setup-header {
  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
  color: white;
  padding: 1.2em;
  text-align: center;
  flex-shrink: 0;
}

.setup-header h1 {
  margin: 0.3em 0 0 0;
  font-size: 1.5em;
  font-weight: 600;
}

.setup-header p {
  margin: 0.3em 0 0 0;
  opacity: 0.9;
  font-size: 0.95em;
}

.setup-content {
  padding: 1.2em 1.5em;
  display: flex;
  flex-direction: column;
  flex: 1;
  overflow: hidden;
}

.step-indicator {
  display: flex;
  justify-content: center;
  align-items: center;
  margin-bottom: 1.2em;
  gap: 0.5em;
  flex-shrink: 0;
}

.step {
  display: flex;
  align-items: center;
  gap: 0.3em;
  font-size: 0.85em;
}

.step-number {
  width: 28px;
  height: 28px;
  border-radius: 50%;
  display: flex;
  align-items: center;
  justify-content: center;
  font-weight: 600;
  font-size: 0.9em;
  background: var(--bs-secondary-bg);
  color: var(--bs-secondary-color);
  transition: all 0.3s ease;
  flex-shrink: 0;
}

.step.active .step-number {
  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
  color: white;
  transform: scale(1.05);
}

.step.completed .step-number {
  background: #28a745;
  color: white;
}

.step-connector {
  width: 30px;
  height: 2px;
  background: var(--bs-secondary-bg);
  flex-shrink: 0;
}

.step-content {
  flex: 1;
  overflow-y: auto;
  overflow-x: hidden;
  padding-right: 0.5em;
}

.step-content h3 {
  font-size: 1.1em;
  margin-bottom: 0.8em;
}

.option-card {
  border: 2px solid var(--bs-border-color);
  border-radius: 10px;
  padding: 0.8em 1em;
  margin-bottom: 0.6em;
  cursor: pointer;
  transition: all 0.3s ease;
  background: var(--bs-body-bg);
}

.option-card:hover {
  border-color: #667eea;
  transform: translateY(-1px);
  box-shadow: 0 3px 10px rgba(102, 126, 234, 0.2);
}

.option-card.selected {
  border-color: #667eea;
  background: rgba(102, 126, 234, 0.1);
}

.option-card .option-icon {
  font-size: 1.8em;
  margin-bottom: 0.3em;
  color: #667eea;
}

.option-card h4 {
  margin: 0.3em 0;
  font-weight: 600;
  font-size: 1em;
}

.option-card p {
  margin: 0;
  color: var(--bs-secondary-color);
  font-size: 0.85em;
  line-height: 1.3;
}

.form-select-large {
  padding: 0.7em;
  font-size: 1em;
  border-radius: 8px;
  border: 2px solid var(--bs-border-color);
  transition: all 0.3s ease;
}

.form-select-large:focus {
  border-color: #667eea;
  box-shadow: 0 0 0 0.2rem rgba(102, 126, 234, 0.25);
}

.action-buttons {
  display: flex;
  justify-content: space-between;
  margin-top: 1em;
  gap: 0.8em;
  flex-shrink: 0;
  padding-top: 1em;
  border-top: 1px solid var(--bs-border-color);
}

.btn-setup {
  padding: 0.6em 1.5em;
  font-size: 1em;
  border-radius: 8px;
  font-weight: 500;
  transition: all 0.3s ease;
}

.btn-setup-primary {
  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
  border: none;
  color: white;
}

.btn-setup-primary:hover:not(:disabled) {
  transform: translateY(-2px);
  box-shadow: 0 4px 12px rgba(102, 126, 234, 0.4);
  color: white;
}

.btn-setup-secondary {
  background: var(--bs-secondary-bg);
  border: none;
  color: var(--bs-body-color);
}

.adapter-info {
  background: var(--bs-secondary-bg);
  padding: 0.8em;
  border-radius: 8px;
  margin-top: 0.8em;
  font-size: 0.9em;
}

.adapter-info h5 {
  font-size: 1em;
  margin-bottom: 0.5em;
}

.adapter-info p {
  margin-bottom: 0.3em;
  font-size: 0.95em;
}

/* 滚动条样式 */
.step-content::-webkit-scrollbar {
  width: 6px;
}

.step-content::-webkit-scrollbar-track {
  background: transparent;
}

.step-content::-webkit-scrollbar-thumb {
  background: rgba(102, 126, 234, 0.3);
  border-radius: 3px;
}

.step-content::-webkit-scrollbar-thumb:hover {
  background: rgba(102, 126, 234, 0.5);
}
</style>

