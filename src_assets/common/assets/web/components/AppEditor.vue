<template>
  <div class="modal fade" id="editAppModal" tabindex="-1" aria-labelledby="editAppModalLabel" ref="modalElement">
    <div class="modal-dialog modal-xl">
      <div class="modal-content">
        <div class="modal-header">
          <h5 class="modal-title" id="editAppModalLabel">
            <i class="fas fa-edit me-2"></i>
            {{ isNewApp ? '添加新应用' : '编辑应用' }}
          </h5>
          <button type="button" class="btn-close" data-bs-dismiss="modal" aria-label="Close"></button>
        </div>
        <div class="modal-body">
          <!-- 隐藏的文件选择输入 -->
          <input type="file" ref="fileInput" style="display: none" />
          <input type="file" ref="dirInput" style="display: none" webkitdirectory />

          <form v-if="formData" @submit.prevent="saveApp">
            <div class="accordion" id="appFormAccordion">
              <!-- 基本信息 -->
              <AccordionItem
                id="basicInfo"
                icon="fa-info-circle"
                title="基本信息"
                parent-id="appFormAccordion"
                :show="true"
              >
                <FormField
                  id="appName"
                  :label="$t('apps.app_name')"
                  :hint="$t('apps.app_name_desc')"
                  :validation="validation.name"
                  :value="formData.name"
                  required
                >
                  <input
                    type="text"
                    class="form-control form-control-enhanced"
                    id="appName"
                    v-model="formData.name"
                    :class="getFieldClass('name')"
                    @blur="validateField('name')"
                    required
                  />
                </FormField>

                <FormField
                  id="appOutput"
                  :label="$t('apps.output_name')"
                  :hint="$t('apps.output_desc')"
                  :validation="validation.output"
                >
                  <input
                    type="text"
                    class="form-control form-control-enhanced monospace"
                    id="appOutput"
                    v-model="formData.output"
                    :class="getFieldClass('output')"
                    @blur="validateField('output')"
                  />
                </FormField>

                <FormField
                  id="appCmd"
                  :label="$t('apps.cmd')"
                  :validation="validation.cmd"
                  :value="formData.cmd"
                  required
                >
                  <template #default>
                    <div class="input-group">
                      <input
                        type="text"
                        class="form-control form-control-enhanced monospace"
                        id="appCmd"
                        v-model="formData.cmd"
                        :class="getFieldClass('cmd')"
                        @blur="validateField('cmd')"
                        :placeholder="getPlaceholderText('cmd')"
                      />
                      <button
                        class="btn btn-outline-secondary"
                        type="button"
                        @click="selectFile('cmd')"
                        :title="getButtonTitle('file')"
                      >
                        <i class="fas fa-folder-open"></i>
                      </button>
                    </div>
                  </template>
                  <template #hint>
                    {{ $t('apps.cmd_desc') }}<br />
                    <strong>{{ $t('_common.note') }}</strong> {{ $t('apps.cmd_note') }}
                  </template>
                </FormField>

                <FormField
                  id="appWorkingDir"
                  :label="$t('apps.working_dir')"
                  :hint="$t('apps.working_dir_desc')"
                  :validation="validation['working-dir']"
                >
                  <div class="input-group">
                    <input
                      type="text"
                      class="form-control form-control-enhanced monospace"
                      id="appWorkingDir"
                      v-model="formData['working-dir']"
                      :class="getFieldClass('working-dir')"
                      @blur="validateField('working-dir')"
                      :placeholder="getPlaceholderText('working-dir')"
                    />
                    <button
                      class="btn btn-outline-secondary"
                      type="button"
                      @click="selectDirectory('working-dir')"
                      :title="getButtonTitle('directory')"
                    >
                      <i class="fas fa-folder-open"></i>
                    </button>
                  </div>
                </FormField>
              </AccordionItem>

              <!-- 命令设置 -->
              <AccordionItem id="commands" icon="fa-terminal" title="命令设置" parent-id="appFormAccordion">
                <div class="form-group-enhanced">
                  <div class="form-check form-switch">
                    <input
                      type="checkbox"
                      class="form-check-input"
                      id="excludeGlobalPrepSwitch"
                      v-model="formData['exclude-global-prep-cmd']"
                      :true-value="'true'"
                      :false-value="'false'"
                    />
                    <label class="form-check-label" for="excludeGlobalPrepSwitch">
                      {{ $t('apps.global_prep_name') }}
                    </label>
                  </div>
                  <div class="field-hint">{{ $t('apps.global_prep_desc') }}</div>
                </div>

                <CommandTable
                  :commands="formData['prep-cmd']"
                  :platform="platform"
                  type="prep"
                  @add-command="addPrepCommand"
                  @remove-command="removePrepCommand"
                  @order-changed="handlePrepCommandOrderChanged"
                />

                <CommandTable
                  :commands="formData['menu-cmd']"
                  :platform="platform"
                  type="menu"
                  @add-command="addMenuCommand"
                  @remove-command="removeMenuCommand"
                  @test-command="testMenuCommand"
                  @order-changed="handleMenuCommandOrderChanged"
                />

                <DetachedCommands
                  :commands="formData.detached"
                  @add-command="addDetachedCommand"
                  @remove-command="removeDetachedCommand"
                />
              </AccordionItem>

              <!-- 高级选项 -->
              <AccordionItem id="advanced" icon="fa-cogs" title="高级选项" parent-id="appFormAccordion">
                <CheckboxField
                  v-if="platform === 'windows'"
                  id="appElevation"
                  v-model="formData.elevated"
                  :label="$t('_common.run_as')"
                  :hint="$t('apps.run_as_desc')"
                />

                <CheckboxField
                  id="autoDetach"
                  v-model="formData['auto-detach']"
                  :label="$t('apps.auto_detach')"
                  :hint="$t('apps.auto_detach_desc')"
                />

                <CheckboxField
                  id="waitAll"
                  v-model="formData['wait-all']"
                  :label="$t('apps.wait_all')"
                  :hint="$t('apps.wait_all_desc')"
                />

                <FormField
                  id="exitTimeout"
                  :label="$t('apps.exit_timeout')"
                  :hint="$t('apps.exit_timeout_desc')"
                  :validation="validation['exit-timeout']"
                >
                  <input
                    type="number"
                    class="form-control form-control-enhanced"
                    id="exitTimeout"
                    v-model="formData['exit-timeout']"
                    min="0"
                    :class="getFieldClass('exit-timeout')"
                    @blur="validateField('exit-timeout')"
                  />
                </FormField>
              </AccordionItem>

              <!-- 图片设置 -->
              <AccordionItem id="image" icon="fa-image" title="图片设置" parent-id="appFormAccordion">
                <ImageSelector
                  :image-path="formData['image-path']"
                  :app-name="formData.name"
                  @update-image="updateImage"
                  @image-error="handleImageError"
                />
              </AccordionItem>
            </div>
          </form>
        </div>
        <div class="modal-footer modal-footer-enhanced">
          <div class="save-status">
            <span v-if="isFormValid" class="text-success"> <i class="fas fa-check-circle me-1"></i>合规应用 </span>
            <span v-else class="text-warning"> <i class="fas fa-exclamation-triangle me-1"></i>请检查必填字段 </span>
            <div v-if="imageError" class="text-danger mt-1">
              <i class="fas fa-exclamation-circle me-1"></i>{{ imageError }}
            </div>
          </div>
          <div>
            <button type="button" class="btn btn-secondary me-2" @click="closeModal">
              <i class="fas fa-times me-1"></i>{{ $t('_common.cancel') }}
            </button>
            <button type="button" class="btn btn-primary" @click="saveApp" :disabled="disabled || !isFormValid">
              <i class="fas fa-save me-1"></i>{{ $t('_common.save') }}
            </button>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script>
import { validateField, validateAppForm } from '../utils/validation.js'
import { nanoid } from 'nanoid'
import CommandTable from './CommandTable.vue'
import DetachedCommands from './DetachedCommands.vue'
import ImageSelector from './ImageSelector.vue'
import AccordionItem from './AccordionItem.vue'
import FormField from './FormField.vue'
import CheckboxField from './CheckboxField.vue'
import { createFileSelector } from '../utils/fileSelection.js'

// 默认表单数据
const DEFAULT_FORM_DATA = {
  name: '',
  output: '',
  cmd: '',
  index: -1,
  'exclude-global-prep-cmd': false,
  elevated: false,
  'auto-detach': true,
  'wait-all': true,
  'exit-timeout': 5,
  'prep-cmd': [],
  'menu-cmd': [],
  detached: [],
  'image-path': '',
  'working-dir': '',
}

// 字段验证映射
const FIELD_VALIDATION_MAP = {
  name: 'appName',
  cmd: 'command',
  output: 'outputName',
  'working-dir': 'workingDir',
  'exit-timeout': 'timeout',
  'image-path': 'imagePath',
}

export default {
  name: 'AppEditor',
  components: {
    CommandTable,
    DetachedCommands,
    ImageSelector,
    AccordionItem,
    FormField,
    CheckboxField,
  },
  props: {
    app: { type: Object, default: null },
    platform: { type: String, default: 'linux' },
    disabled: { type: Boolean, default: false },
  },
  emits: ['close', 'save-app'],
  data() {
    return {
      formData: null,
      validation: {},
      imageError: '',
      modalInstance: null,
      fileSelector: null,
    }
  },
  computed: {
    isNewApp() {
      return !this.app || this.app.index === -1
    },
    isFormValid() {
      return this.validation.name?.isValid && this.validation.cmd?.isValid
    },
  },
  watch: {
    app: {
      handler(newApp) {
        if (newApp) {
          this.initializeForm(newApp)
          this.$nextTick(this.showModal)
        }
      },
      immediate: true,
    },
  },
  mounted() {
    this.$nextTick(() => {
      this.initializeModal()
      this.initializeFileSelector()
    })
  },
  beforeUnmount() {
    this.cleanup()
  },
  methods: {
    // 初始化方法
    initializeModal() {
      if (this.modalInstance || !this.$refs.modalElement) return

      const Modal = window.bootstrap?.Modal
      if (!Modal) {
        console.warn('Bootstrap Modal not available')
        return
      }

      try {
        this.modalInstance = new Modal(this.$refs.modalElement, {
          backdrop: 'static',
          keyboard: false,
        })
        this.$refs.modalElement.addEventListener('hidden.bs.modal', this.restoreBodyScroll)
      } catch (error) {
        console.warn('Modal initialization failed:', error)
      }
    },

    initializeFileSelector() {
      const notify = (type) => (message) => this.showMessage(message, type)
      this.fileSelector = createFileSelector({
        platform: this.platform,
        onSuccess: notify('info'),
        onError: notify('error'),
        onInfo: notify('info'),
      })
    },

    initializeForm(app) {
      this.formData = { ...DEFAULT_FORM_DATA, ...JSON.parse(JSON.stringify(app)) }
      this.ensureDefaultValues()
      this.validation = {}
      this.imageError = ''
    },

    ensureDefaultValues() {
      const defaults = {
        'prep-cmd': [],
        'menu-cmd': [],
        detached: [],
        'exclude-global-prep-cmd': false,
        'working-dir': '',
      }

      Object.entries(defaults).forEach(([key, value]) => {
        if (!this.formData[key]) this.formData[key] = value
      })

      const conditionalDefaults = {
        elevated: this.platform === 'windows' ? false : undefined,
        'auto-detach': true,
        'wait-all': true,
        'exit-timeout': 5,
      }

      Object.entries(conditionalDefaults).forEach(([key, value]) => {
        if (this.formData[key] === undefined && value !== undefined) {
          this.formData[key] = value
        }
      })
    },

    // 模态框操作
    showModal() {
      if (!this.modalInstance) this.initializeModal()
      this.modalInstance?.show()
    },

    closeModal() {
      this.modalInstance?.hide()
      this.resetFileSelection()
      this.restoreBodyScroll()
      this.$emit('close')
    },

    cleanup() {
      this.modalInstance?.dispose()
      if (this.fileSelector) {
        this.fileSelector.resetState()
        this.fileSelector.cleanupFileInputs(this.$refs.fileInput, this.$refs.dirInput)
      }
      this.restoreBodyScroll()
    },

    resetFileSelection() {
      if (this.fileSelector) {
        this.fileSelector.resetState()
        this.fileSelector.cleanupFileInputs(this.$refs.fileInput, this.$refs.dirInput)
      }
    },

    restoreBodyScroll() {
      Object.assign(document.body.style, { overflow: '', paddingRight: '' })
      document.querySelectorAll('.modal-backdrop').forEach((el) => el.remove())
    },

    // 消息提示
    showMessage(message, type = 'info') {
      if (window.showToast) {
        window.showToast(message, type)
      } else {
        type === 'error' ? alert(message) : console.info(message)
      }
    },

    // 验证方法
    validateField(fieldName) {
      const validationKey = FIELD_VALIDATION_MAP[fieldName] || fieldName
      const result = validateField(validationKey, this.formData[fieldName])
      this.validation[fieldName] = result
      return result
    },

    getFieldClass(fieldName) {
      const v = this.validation[fieldName]
      if (!v) return ''
      return { 'is-invalid': !v.isValid, 'is-valid': v.isValid && this.formData[fieldName] }
    },

    // 命令操作
    addPrepCommand() {
      const cmd = { do: '', undo: '' }
      if (this.platform === 'windows') cmd.elevated = false
      this.formData['prep-cmd'].push(cmd)
    },

    removePrepCommand(index) {
      this.formData['prep-cmd'].splice(index, 1)
    },

    addMenuCommand() {
      const cmd = { id: nanoid(10), name: '', cmd: '' }
      if (this.platform === 'windows') cmd.elevated = false
      this.formData['menu-cmd'].push(cmd)
    },

    removeMenuCommand(index) {
      this.formData['menu-cmd'].splice(index, 1)
    },

    handlePrepCommandOrderChanged(newOrder) {
      this.formData['prep-cmd'] = newOrder
    },

    handleMenuCommandOrderChanged(newOrder) {
      this.formData['menu-cmd'] = newOrder
    },

    async testMenuCommand(index) {
      const menuCmd = this.formData['menu-cmd'][index]
      if (!menuCmd.cmd) {
        this.showMessage(this.$t('apps.test_menu_cmd_empty'), 'error')
        return
      }

      try {
        this.showMessage(this.$t('apps.test_menu_cmd_executing'))
        const response = await fetch('/api/apps/test-menu-cmd', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            cmd: menuCmd.cmd,
            working_dir: this.formData['working-dir'] || '',
            elevated: menuCmd.elevated === 'true' || menuCmd.elevated === true,
          }),
        })

        const result = await response.json()
        this.showMessage(
          result.status
            ? this.$t('apps.test_menu_cmd_success')
            : `${this.$t('apps.test_menu_cmd_failed')}: ${result.error || 'Unknown error'}`,
          result.status ? 'info' : 'error'
        )
      } catch (error) {
        this.showMessage(`${this.$t('apps.test_menu_cmd_failed')}: ${error.message}`, 'error')
      }
    },

    addDetachedCommand() {
      this.formData.detached.push('')
    },

    removeDetachedCommand(index) {
      this.formData.detached.splice(index, 1)
    },

    // 图片操作
    updateImage(imagePath) {
      this.formData['image-path'] = imagePath
      this.imageError = ''
    },

    handleImageError(error) {
      this.imageError = error
    },

    // 文件选择
    selectFile(fieldName) {
      if (!this.fileSelector) {
        this.showMessage('文件选择器未初始化', 'error')
        return
      }
      this.fileSelector.selectFile(fieldName, this.$refs.fileInput, this.onFilePathSelected)
    },

    selectDirectory(fieldName) {
      if (!this.fileSelector) {
        this.showMessage('文件选择器未初始化', 'error')
        return
      }
      this.fileSelector.selectDirectory(fieldName, this.$refs.dirInput, this.onFilePathSelected)
    },

    onFilePathSelected(fieldName, filePath) {
      this.formData[fieldName] = filePath
      this.validateField(fieldName)
    },

    getPlaceholderText(fieldName) {
      return this.fileSelector?.getPlaceholderText(fieldName) || ''
    },

    getButtonTitle(type) {
      return this.fileSelector?.getButtonTitle(type) || '选择'
    },

    // 保存
    async saveApp() {
      const formValidation = validateAppForm(this.formData)
      if (!formValidation.isValid) {
        if (formValidation.errors.length) alert(formValidation.errors[0])
        return
      }

      const editedApp = {
        ...this.formData,
        ...(this.formData['image-path'] && {
          'image-path': this.formData['image-path'].toString().replace(/"/g, ''),
        }),
      }

      this.$emit('save-app', editedApp)
    },
  },
}
</script>

<style scoped>
.modal-footer-enhanced {
  border-top: 1px solid #dee2e6;
  padding: 1rem 1.5rem;
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.save-status {
  font-size: 0.875rem;
  color: #6c757d;
}

.is-invalid {
  border-color: #dc3545;
}

.is-valid {
  border-color: #198754;
}

.monospace {
  font-family: monospace;
}
</style>
