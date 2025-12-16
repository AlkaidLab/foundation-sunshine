<template>
  <div class="setup-container">
    <div class="setup-card">
      <div class="setup-header">
        <img src="/images/logo-sunshine-256.png" height="60" alt="Sunshine">
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

          <!-- 步骤 2: 选择串流显示器 -->
          <div v-else-if="currentStep === 2">
            <h3 class="mb-4">{{ $t('setup.step1_description') }}</h3>
            
            <!-- 虚拟显示器选项 -->
            <div class="option-card" 
                 :class="{ selected: selectedDisplay === 'ZakoHDR' }"
                 @click="selectedDisplay = 'ZakoHDR'">
              <div class="option-icon">
                <i class="fas fa-tv"></i>
              </div>
              <h4>{{ $t('setup.virtual_display') }}</h4>
              <p>{{ $t('setup.virtual_display_desc') }}</p>
            </div>

            <!-- 物理显示器列表 -->
            <div v-if="displayDevices && displayDevices.length > 0">
              <h5 class="my-3">
                <i class="fas fa-desktop"></i>
                {{ $t('setup.physical_display') }}
              </h5>
              <div class="option-card" 
                   v-for="device in displayDevices" 
                   :key="device.device_id"
                   :class="{ selected: selectedDisplay === device.device_id }"
                   @click="selectedDisplay = device.device_id">
                <div class="d-flex align-items-center">
                  <div class="option-icon-small">
                    <i class="fas fa-desktop"></i>
                  </div>
                  <div class="flex-grow-1">
                    <h4>{{ getDisplayName(device) }}</h4>
                    <p>{{ getDisplayInfo(device) }}</p>
                  </div>
                </div>
              </div>
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
                <p><strong>{{ $t('setup.selected_display') }}:</strong> 
                  {{ isVirtualDisplay ? $t('setup.virtual_display') : selectedDisplay }}
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
              <div class="mb-3" style="font-size: 3em; color: #28a745;">
                <i class="fas fa-check-circle"></i>
              </div>
              <h3 class="mb-2">{{ $t('setup.setup_complete') }}</h3>
              <p class="mb-3">{{ $t('setup.setup_complete_desc') }}</p>
              
              <div class="alert alert-info" v-if="saveSuccess">
                <i class="fas fa-info-circle"></i>
                {{ $t('setup.config_saved') }}
              </div>
              
              <div class="alert alert-danger" v-if="saveError">
                <i class="fas fa-exclamation-triangle"></i>
                {{ saveError }}
              </div>

              <!-- 客户端下载二维码 -->
              <div class="client-download-section mt-3">
                <h5 class="mb-3">
                  <i class="fas fa-mobile-alt"></i>
                  {{ $t('setup.download_clients') }}
                </h5>
                <div class="qr-codes-container">
                  <div class="qr-code-item">
                    <div class="qr-code-box">
                      <img :src="androidQrCode" alt="Android QR Code" class="qr-code-image">
                    </div>
                    <div class="qr-code-label">
                      <i class="fab fa-android"></i>
                      {{ $t('setup.android_client') }}
                    </div>
                  </div>
                  <div class="qr-code-item">
                    <div class="qr-code-box">
                      <img :src="iosQrCode" alt="iOS QR Code" class="qr-code-image">
                    </div>
                    <div class="qr-code-label">
                      <i class="fab fa-apple"></i>
                      {{ $t('setup.ios_client') }}
                    </div>
                  </div>
                </div>
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
          <button class="btn btn-setup btn-setup-skip" 
                  @click="skipWizard" 
                  v-if="currentStep < 5"
                  :disabled="saving"
                  type="button">
            <i class="fas fa-forward"></i>
            {{ $t('setup.skip') }}
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
    <!-- Skip Wizard Modal -->
    <Transition name="fade">
      <div v-if="showSkipModal" class="skip-wizard-overlay" @click.self="closeSkipModal">
        <div class="skip-wizard-modal">
          <div class="skip-wizard-header">
            <h5>{{ $t('setup.skip_confirm_title')}}</h5>
            <button class="btn-close" @click="closeSkipModal"></button>
          </div>
          <div class="skip-wizard-body">
            <p>{{ $t('setup.skip_confirm') }}</p>
          </div>
          <div class="skip-wizard-footer">
            <button type="button" class="btn btn-secondary" @click="closeSkipModal">{{ $t('_common.cancel') }}</button>
            <button type="button" class="btn btn-warning" @click="confirmSkipWizard">{{ $t('setup.skip') }}</button>
          </div>
        </div>
      </div>
    </Transition>
  </div>
</template>

<script>
import { trackEvents } from '../config/firebase.js'

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
      selectedDisplay: null, // 选择的显示器（虚拟或物理）
      selectedAdapter: '',
      displayDevicePrep: 'ensure_only_display', // 默认选择：确保唯一显示器
      saveSuccess: false,
      saveError: null,
      saving: false,
      showSkipModal: false, // 跳过向导确认弹窗
      // 客户端下载链接
      androidQrCode: 'https://api.qrserver.com/v1/create-qr-code/?size=150x150&data=' + encodeURIComponent('https://github.com/qiin2333/moonlight-android/releases/tag/shortcut'),
      iosQrCode: 'https://api.qrserver.com/v1/create-qr-code/?size=150x150&data=' + encodeURIComponent('https://apps.apple.com/us/app/voidlink/id6747717070'),
    }
  },
  setup() {
    return {}
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
        return this.selectedDisplay !== null
      } else if (this.currentStep === 3) {
        return this.selectedAdapter !== null
      } else if (this.currentStep === 4) {
        return this.displayDevicePrep !== null
      }
      return false
    },
    isVirtualDisplay() {
      return this.selectedDisplay === 'ZakoHDR'
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
        // 先获取当前完整配置，保留所有已有设置
        const currentConfig = await fetch('/api/config').then(r => r.json())
        
        // 从完整配置中复制所有字段，避免覆盖其他配置
        const config = { ...currentConfig }

        // 标记新手引导已完成
        config.setup_wizard_completed = true
        
        // 确保 locale 被保存（如果用户在步骤1选择了语言，或者已有配置中有 locale）
        if (this.selectedLocale) {
          config.locale = this.selectedLocale
        } else if (currentConfig.locale) {
          config.locale = currentConfig.locale
        }
        
        // 设置 adapter_name
        config.adapter_name = this.selectedAdapter || ''

        // 设置选择的显示器
        config.output_name = this.selectedDisplay

        // 添加显示器组合策略
        config.display_device_prep = this.displayDevicePrep

        console.log('保存配置:', config)

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
            selected_display: this.selectedDisplay,
            adapter: this.selectedAdapter,
            display_device_prep: this.displayDevicePrep,
            is_virtual_display: this.isVirtualDisplay
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
    skipWizard(event) {
      if (event) {
        event.preventDefault()
        event.stopPropagation()
      }
      
      if (this.saving) return
      
      this.openSkipModal()
    },
    openSkipModal() {
      this.showSkipModal = true
    },
    closeSkipModal() {
      this.showSkipModal = false
    },
    async confirmSkipWizard() {
      // 关闭模态框
      this.closeSkipModal()
      
      if (this.saving) return

      this.saving = true
      this.saveError = null

      try {
        // 先获取当前完整配置，保留所有已有设置
        const currentConfig = await fetch('/api/config').then(r => r.json())
        
        // 从完整配置中复制所有字段，避免覆盖其他配置
        const config = { ...currentConfig }
        // 标记新手引导已完成
        config.setup_wizard_completed = true
        console.log('跳过新手引导，保存配置:', config)
        const response = await fetch('/api/config', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json',
          },
          body: JSON.stringify(config),
        })

        if (response.ok) {
          // 记录跳过事件
          trackEvents.userAction('setup_wizard_skipped', {
            from_step: this.currentStep
          })
          
          // 触发完成事件，让父组件知道设置向导已完成
          this.$emit('setup-complete', config)
          
          // 重新加载页面以隐藏设置向导
          window.location.reload()
        } else {
          const errorText = await response.text()
          this.saveError = `${this.$t('setup.skip_error')}: ${errorText}`
          
          // 记录跳过失败
          trackEvents.errorOccurred('setup_wizard_skip_failed', errorText)
        }
      } catch (error) {
        console.error('Failed to skip wizard:', error)
        this.saveError = `${this.$t('setup.skip_error')}: ${error.message}`
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
    },
    getDisplayName(device) {
      // 解析 device.data，提取友好名称
      // 数据格式：
      // DISPLAY NAME: \\.\\DISPLAY1
      // FRIENDLY NAME: F32D80U
      // DEVICE STATE: PRIMARY
      // HDR STATE: ENABLED
      try {
        const data = device.data || ''
        const name = data
          .replace(
            /.*?(DISPLAY\d+)?\nFRIENDLY NAME: (.*[^\n])*?\n.*\n.*/g,
            "$2 ($1)"
          )
          .replace("()", "")
        
        return name || device.device_id || this.$t('setup.unknown_display')
      } catch (e) {
        return device.device_id || this.$t('setup.unknown_display')
      }
    },
    getDisplayInfo(device) {
      // 解析 device.data，提取详细信息
      try {
        const data = device.data || ''
        
        // 提取 DEVICE STATE
        const stateMatch = data.match(/DEVICE STATE: (\w+)/)
        const state = stateMatch ? stateMatch[1].toLowerCase() : 'unknown'
        
        // 提取 HDR STATE
        const hdrMatch = data.match(/HDR STATE: (\w+)/)
        const hdr = hdrMatch ? hdrMatch[1] : ''
        
        const stateKey = {
          'primary': 'setup.state_primary',
          'active': 'setup.state_active',
          'inactive': 'setup.state_inactive'
        }[state] || 'setup.state_unknown'
        const stateText = this.$t(stateKey)
        
        let info = `${this.$t('setup.device_state')}: ${stateText}`
        if (hdr) {
          info += ` | HDR: ${hdr}`
        }
        
        return info
      } catch (e) {
        return device.device_id
      }
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

.btn-setup-secondary:hover:not(:disabled) {
  background: var(--bs-tertiary-bg);
  transform: translateY(-1px);
}

.btn-setup-skip {
  background: rgba(255, 255, 255, 0.95);
  border: 2px solid rgba(102, 126, 234, 0.7);
  color: rgba(70, 90, 200, 1);
  font-weight: 500;
}

.btn-setup-skip:hover:not(:disabled) {
  background: rgba(255, 255, 255, 1);
  border-color: rgba(102, 126, 234, 0.9);
  color: rgba(50, 70, 180, 1);
  transform: translateY(-1px);
  box-shadow: 0 2px 8px rgba(102, 126, 234, 0.3);
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

/* 客户端下载二维码样式 */
.client-download-section {
  background: var(--bs-secondary-bg);
  padding: 1em;
  border-radius: 10px;
}

.client-download-section h5 {
  font-size: 1em;
  margin-bottom: 0.8em;
  color: var(--bs-body-color);
}

.qr-codes-container {
  display: flex;
  justify-content: center;
  gap: 2em;
  flex-wrap: wrap;
}

.qr-code-item {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 0.5em;
}

.qr-code-box {
  background: white;
  padding: 0.5em;
  border-radius: 8px;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.1);
}

.qr-code-image {
  width: 120px;
  height: 120px;
  display: block;
}

.qr-code-label {
  font-size: 0.9em;
  font-weight: 500;
  color: var(--bs-body-color);
}

.qr-code-label i {
  margin-right: 0.3em;
}

/* 小图标样式 */
.option-icon-small {
  font-size: 1.5em;
  color: #667eea;
  margin-right: 0.8em;
  flex-shrink: 0;
}

.d-flex {
  display: flex;
}

.align-items-center {
  align-items: center;
}

.flex-grow-1 {
  flex-grow: 1;
}

.my-3 {
  margin-top: 1rem;
  margin-bottom: 1rem;
}

/* Skip Wizard Modal - 使用 ScanResultModal 样式 */
.skip-wizard-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  width: 100vw;
  height: 100vh;
  margin: 0;
  background: var(--overlay-bg, rgba(0, 0, 0, 0.7));
  backdrop-filter: blur(8px);
  z-index: 9999;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: var(--spacing-lg, 20px);
  overflow: hidden;
  
  [data-bs-theme='light'] & {
    background: rgba(0, 0, 0, 0.5);
  }
}

.skip-wizard-modal {
  background: var(--modal-bg, rgba(30, 30, 50, 0.95));
  border: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.2));
  border-radius: var(--border-radius-xl, 12px);
  width: 100%;
  max-width: 500px;
  max-height: 80vh;
  display: flex;
  flex-direction: column;
  backdrop-filter: blur(20px);
  box-shadow: var(--shadow-xl, 0 25px 50px rgba(0, 0, 0, 0.5));
  animation: modalSlideUp 0.3s ease;
  
  [data-bs-theme='light'] & {
    background: rgba(255, 255, 255, 0.95);
    border: 1px solid rgba(0, 0, 0, 0.15);
    box-shadow: 0 25px 50px rgba(0, 0, 0, 0.2);
  }
}

@keyframes modalSlideUp {
  from {
    transform: translateY(20px);
    opacity: 0;
  }
  to {
    transform: translateY(0);
    opacity: 1;
  }
}

.skip-wizard-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: var(--spacing-md, 20px) var(--spacing-lg, 24px);
  border-bottom: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.1));

  h5 {
    margin: 0;
    color: var(--text-primary, #fff);
    font-size: var(--font-size-lg, 1.1rem);
    font-weight: 600;
    display: flex;
    align-items: center;
    gap: var(--spacing-sm, 8px);
  }
  
  [data-bs-theme='light'] & {
    border-bottom: 1px solid rgba(0, 0, 0, 0.1);
    
    h5 {
      color: #000000;
    }
  }
}

.skip-wizard-body {
  padding: var(--spacing-lg, 24px);
  font-size: var(--font-size-md, 0.95rem);
  line-height: 1.5;
  overflow-y: auto;
  flex: 1;
  color: var(--text-primary, #fff);
  
  [data-bs-theme='light'] & {
    color: #000000;
  }
}

.skip-wizard-footer {
  display: flex;
  justify-content: flex-end;
  gap: 10px;
  padding: var(--spacing-md, 20px) var(--spacing-lg, 24px);
  border-top: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.1));
  
  [data-bs-theme='light'] & {
    border-top: 1px solid rgba(0, 0, 0, 0.1);
  }
}

.skip-wizard-footer button {
  padding: 8px 16px;
  font-size: 0.9rem;
}

/* Vue 过渡动画 */
.fade-enter-active {
  transition: opacity 0.3s ease;
}

.fade-leave-active {
  transition: opacity 0.3s ease;
}

.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}
</style>

