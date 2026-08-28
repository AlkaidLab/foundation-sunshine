<template>
  <div class="modal fade" id="editAppModal" tabindex="-1" aria-labelledby="editAppModalLabel" ref="modalElement">
    <div class="modal-dialog modal-xl">
      <div class="modal-content">
        <div class="modal-header">
          <h5 class="modal-title" id="editAppModalLabel">
            <i class="fas fa-edit me-2"></i>
            {{ isNewApp ? t('apps.add_new') : t('apps.edit') }}
          </h5>
          <button type="button" class="btn-close" data-bs-dismiss="modal" aria-label="Close"></button>
        </div>
        <div class="modal-body">
          <input type="file" ref="fileInput" style="display: none" />
          <input type="file" ref="dirInput" style="display: none" webkitdirectory />

          <form v-if="formData" @submit.prevent="saveApp">
            <div class="accordion" id="appFormAccordion">
              <AccordionItem
                id="basicInfo"
                icon="fa-info-circle"
                :title="t('apps.basic_info')"
                parent-id="appFormAccordion"
                :show="true"
              >
                <FormField
                  id="appName"
                  :label="t('apps.app_name')"
                  :hint="t('apps.app_name_desc')"
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
                  :label="t('apps.output_name')"
                  :hint="t('apps.output_desc')"
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
                  :label="t('apps.cmd')"
                  :validation="validation.cmd"
                  :value="formData.cmd"
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
                        @input="handleCmdInput"
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
                    {{ t('apps.cmd_desc') }}<br />
                    <strong>{{ t('_common.note') }}</strong> {{ t('apps.cmd_note') }}<br />
                    <div class="cmd-examples">
                      <div class="cmd-examples-header"><i class="fas fa-lightbulb me-1"></i>{{ t('apps.cmd_examples_title') }}</div>
                      <div class="cmd-examples-tags">
                        <span class="cmd-tag">
                          <code>cmd /c "start xbox:"</code>
                          <span class="cmd-tag-desc">Xbox Game</span>
                        </span>
                        <span class="cmd-tag">
                          <code>steam://open/bigpicture</code>
                          <span class="cmd-tag-desc">Steam Big Picture</span>
                        </span>
                        <span class="cmd-tag">
                          <code>cmd /c "start ms-gamebar:"</code>
                          <span class="cmd-tag-desc">Xbox Game Bar</span>
                        </span>
                        <span class="cmd-tag">
                          <code>cmd /c "start playnite://playnite/showMainWindow"</code>
                          <span class="cmd-tag-desc">Playnite</span>
                        </span>
                        <span class="cmd-tag">
                          <code>"C:\Program Files\...\game.exe"</code>
                          <span class="cmd-tag-desc">Start program directly</span>
                        </span>
                      </div>
                    </div>
                  </template>
                </FormField>

                <FormField
                  id="appWorkingDir"
                  :label="t('apps.working_dir')"
                  :hint="t('apps.working_dir_desc')"
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

              <AccordionItem
                v-if="isWindows"
                id="displayProfile"
                icon="fa-display"
                :title="t('apps.display_profile_title')"
                parent-id="appFormAccordion"
              >
                <NewDisplayOutputSelector
                  v-model="appDisplayOutput"
                  context="app"
                  :platform="platform"
                  :displays="displayDevices"
                />

                <div v-if="showPhysicalGlobalVddWarning" class="alert alert-warning display-profile-alert" role="alert">
                  <i class="fas fa-triangle-exclamation me-2"></i>
                  {{ t('apps.display_profile_physical_global_vdd_warning') }}
                </div>
                <div class="form-text" style="margin-top: .35rem;">
                  {{ t('apps.display_profile_dual_gpu_hint') }}
                </div>

                <template v-if="hasForcedDisplayProfile">
                  <div v-if="formData['display-target'] === 'virtual' && formData['display-device-prep'] === 'ensure_only_display'" class="alert alert-warning display-profile-alert" role="alert">
                    <i class="fas fa-triangle-exclamation me-2"></i>
                    {{ t('apps.display_profile_exclusive_warning') }}
                  </div>

                  <DisplayPreparationPicker v-model="formData['display-device-prep']" />

                  <details class="display-options-note">
                    <summary>
                      <i class="fas fa-circle-info" aria-hidden="true"></i>
                      {{ tp('config.display_device_options_note') }}
                    </summary>
                    <p class="pre-line">{{ tp('config.display_device_options_note_desc') }}</p>
                  </details>

                  <div class="display-rule-grid">
                    <DisplayRuleRadioGroup
                      v-model="appResolutionRule"
                      name="app_resolution_change"
                      label-key="apps.display_profile_resolution"
                      option-key-prefix="apps.display_profile_"
                      :options="['inherit', 'no_operation', 'follow_client']"
                      :option-labels="appResolutionOptionLabels"
                      :platform-aware="false"
                      :disabled="hasFixedResolution"
                    >
                      <div class="form-text">{{ resHintText }}</div>
                    </DisplayRuleRadioGroup>

                    <DisplayRuleRadioGroup
                      v-model="appRefreshRateRule"
                      name="app_refresh_rate_change"
                      label-key="apps.display_profile_refresh_rate"
                      option-key-prefix="apps.display_profile_"
                      :options="['inherit', 'follow_client']"
                      :option-labels="appRefreshRateOptionLabels"
                      :platform-aware="false"
                      :disabled="hasFixedRefreshRate"
                    >
                      <div class="form-text">{{ rrHintText }}</div>
                    </DisplayRuleRadioGroup>
                  </div>

                  <!-- Advanced options: server-side fixed values (implementation notes in the UI) -->
                  <details class="advanced-options-note">
                    <summary>
                      <i class="fas fa-flask" aria-hidden="true"></i>
                      {{ t('apps.display_profile_advanced_title') }}
                      <span class="advanced-sub">{{ t('apps.display_profile_advanced_sub') }}</span>
                    </summary>
                    <div class="advanced-body">
                      <div class="form-check form-switch">
                        <input
                          type="checkbox"
                          class="form-check-input"
                          id="app_fixed_values_switch"
                          v-model="fixedValuesEnabled"
                        />
                        <label class="form-check-label" for="app_fixed_values_switch">
                          {{ t('apps.display_profile_fixed_resolution') }} / {{ t('apps.display_profile_fixed_refresh_rate') }}
                        </label>
                      </div>
                      <div class="field-row">
                        <input
                          type="text"
                          class="form-control monospace"
                          id="app_fixed_resolution"
                          placeholder="2560x1440"
                          v-model.trim="formData['display-resolution']"
                          :disabled="!fixedValuesEnabled"
                        />
                        <span class="field-sep">@</span>
                        <input
                          type="text"
                          class="form-control monospace"
                          id="app_fixed_refresh_rate"
                          placeholder="60"
                          v-model.trim="formData['display-refresh-rate']"
                          :disabled="!fixedValuesEnabled"
                        />
                      </div>
                      <div class="impl-note">{{ t('apps.display_profile_advanced_res_note') }}</div>

                      <div class="field-row hdr-row">
                        <label class="form-label" for="app_fixed_hdr">{{ t('apps.display_profile_fixed_hdr') }}</label>
                        <select class="form-select" id="app_fixed_hdr" v-model="formData['display-hdr']">
                          <option value="">{{ t('apps.display_profile_fixed_hdr_inherit') }}</option>
                          <option value="on">{{ t('apps.display_profile_fixed_hdr_on') }}</option>
                          <option value="off">{{ t('apps.display_profile_fixed_hdr_off') }}</option>
                        </select>
                      </div>
                      <div class="impl-note">{{ t('apps.display_profile_advanced_hdr_note') }}</div>
                    </div>
                  </details>
                </template>
              </AccordionItem>

              <AccordionItem id="commands" icon="fa-terminal" :title="t('apps.command_settings')" parent-id="appFormAccordion">
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
                      {{ t('apps.global_prep_name') }}
                    </label>
                  </div>
                  <div class="field-hint">{{ t('apps.global_prep_desc') }}</div>
                </div>

                <div class="form-group-enhanced">
                  <label class="form-label-enhanced">{{ t('apps.cmd_prep_name') }}</label>
                  <div class="field-hint mb-3">{{ t('apps.cmd_prep_desc') }}</div>
                  <CommandTable
                    :commands="formData['prep-cmd']"
                    :platform="platform"
                    type="prep"
                    @add-command="addPrepCommand"
                    @remove-command="removePrepCommand"
                    @order-changed="handlePrepCommandOrderChanged"
                  />
                </div>

                <div class="form-group-enhanced">
                  <label class="form-label-enhanced">{{ t('apps.menu_cmd_name') }}</label>
                  <div class="field-hint mb-3">{{ t('apps.menu_cmd_desc') }}</div>
                  <CommandTable
                    :commands="formData['menu-cmd']"
                    :platform="platform"
                    type="menu"
                    @add-command="addMenuCommand"
                    @remove-command="removeMenuCommand"
                    @test-command="testMenuCommand"
                    @order-changed="handleMenuCommandOrderChanged"
                  />
                </div>

                <div class="form-group-enhanced">
                  <label class="form-label-enhanced">{{ t('apps.detached_cmds') }}</label>
                  <div class="field-hint mb-3">
                    {{ t('apps.detached_cmds_desc') }}<br>
                    <strong>{{ t('_common.note') }}</strong> {{ t('apps.detached_cmds_note') }}
                  </div>
                  <CommandTable
                    :commands="formData.detached"
                    :platform="platform"
                    type="detached"
                    @add-command="addDetachedCommand"
                    @remove-command="removeDetachedCommand"
                    @order-changed="handleDetachedCommandOrderChanged"
                  />
                </div>
              </AccordionItem>

              <AccordionItem id="advanced" icon="fa-cogs" :title="t('apps.advanced_options')" parent-id="appFormAccordion">
                <CheckboxField
                  v-if="isWindows"
                  id="appElevation"
                  v-model="formData.elevated"
                  :label="t('_common.run_as')"
                  :hint="t('apps.run_as_desc')"
                />

                <FormField
                  v-if="isWindows"
                  id="gamepadMode"
                  :label="t('apps.gamepad_mode')"
                  :hint="t('apps.gamepad_mode_desc')"
                >
                  <select
                    id="gamepadMode"
                    class="form-select form-control-enhanced"
                    v-model="formData.gamepad"
                  >
                    <option
                      v-for="mode in PER_APP_GAMEPAD_MODES"
                      :key="mode.value"
                      :value="mode.value"
                    >
                      {{ t(mode.labelKey) }}
                    </option>
                  </select>
                </FormField>

                <FormField
                  v-if="isWindows"
                  id="mouseMode"
                  :label="t('apps.mouse_mode')"
                  :hint="t('apps.mouse_mode_desc')"
                >
                  <select
                    id="mouseMode"
                    class="form-select form-control-enhanced"
                    v-model="formData['mouse-mode']"
                  >
                    <option :value="0">{{ t('apps.mouse_mode_auto') }}</option>
                    <option :value="1">{{ t('apps.mouse_mode_vmouse') }}</option>
                    <option :value="2">{{ t('apps.mouse_mode_sendinput') }}</option>
                  </select>
                </FormField>

                <CheckboxField
                  id="autoDetach"
                  v-model="formData['auto-detach']"
                  :label="t('apps.auto_detach')"
                  :hint="t('apps.auto_detach_desc')"
                />

                <CheckboxField
                  id="waitAll"
                  v-model="formData['wait-all']"
                  :label="t('apps.wait_all')"
                  :hint="t('apps.wait_all_desc')"
                />

                <FormField
                  id="exitTimeout"
                  :label="t('apps.exit_timeout')"
                  :hint="t('apps.exit_timeout_desc')"
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

              <AccordionItem id="image" icon="fa-image" :title="t('apps.image_settings')" parent-id="appFormAccordion">
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
            <span v-if="isFormValid" class="text-success"> <i class="fas fa-check-circle me-1"></i>{{ t('apps.form_valid') }} </span>
            <span v-else class="text-warning"> <i class="fas fa-exclamation-triangle me-1"></i>{{ t('apps.form_invalid') }} </span>
            <div v-if="imageError" class="text-danger mt-1">
              <i class="fas fa-exclamation-circle me-1"></i>{{ imageError }}
            </div>
          </div>
          <div>
            <button type="button" class="btn btn-secondary me-2" @click="closeModal">
              <i class="fas fa-times me-1"></i>{{ t('_common.cancel') }}
            </button>
            <button type="button" class="btn btn-primary" @click="saveApp" :disabled="disabled || !isFormValid">
              <i class="fas fa-save me-1"></i>{{ t('_common.save') }}
            </button>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, watch, onMounted, onBeforeUnmount, nextTick, provide } from 'vue'
import { useI18n } from 'vue-i18n'
import { usePlatformI18n } from '../platform-i18n'
import { validateField as validateFieldHelper, validateAppForm } from '../utils/validation.js'
import { nanoid } from 'nanoid'
import CommandTable from './CommandTable.vue'
import ImageSelector from './ImageSelector.vue'
import AccordionItem from './AccordionItem.vue'
import FormField from './FormField.vue'
import CheckboxField from './CheckboxField.vue'
import DisplayPreparationPicker from '../configs/tabs/audiovideo/DisplayPreparationPicker.vue'
import DisplayRuleRadioGroup from '../configs/tabs/audiovideo/DisplayRuleRadioGroup.vue'
import NewDisplayOutputSelector from '../configs/tabs/audiovideo/NewDisplayOutputSelector.vue'
import { createFileSelector } from '../utils/fileSelection.js'
import { apiJson, apiPostJson } from '../utils/apiFetch.js'
import { deepClone } from '../utils/helpers.js'
import { normalizeAppDisplayProfile } from '../utils/appDisplayProfile.js'
import { PER_APP_GAMEPAD_MODES } from '../utils/gamepadModes.js'

const DEFAULT_FORM_DATA = Object.freeze({
  name: '',
  output: '',
  cmd: '',
  index: -1,
  'exclude-global-prep-cmd': false,
  elevated: false,
  'auto-detach': true,
  'wait-all': true,
  'exit-timeout': 5,
  'mouse-mode': 0,
  gamepad: '',
  'prep-cmd': [],
  'menu-cmd': [],
  detached: [],
  'image-path': '',
  'working-dir': '',
  'display-target': '',
  'display-device-prep': 'ensure_active',
  'display-resolution-mode': '',
  'display-refresh-rate-mode': '',
  'display-output-name': '',
  'display-resolution': '',
  'display-refresh-rate': '',
  'display-hdr': '',
})

const FIELD_VALIDATION_MAP = Object.freeze({
  name: 'appName',
  cmd: 'command',
  output: 'outputName',
  'working-dir': 'workingDir',
  'exit-timeout': 'timeout',
  'image-path': 'imagePath',
})

const props = defineProps({
  app: { type: Object, default: null },
  platform: { type: String, default: 'linux' },
  disabled: { type: Boolean, default: false },
})

const emit = defineEmits(['close', 'save-app'])

// Provide the platform to the shared display components (they call
// usePlatformI18n() which reads the injected 'platform').
provide(
  'platform',
  computed(() => props.platform)
)

const { t } = useI18n()
const platformMessage = usePlatformI18n(props.platform)
const tp = (key) => platformMessage.getMessageUsingPlatform(key)

const modalElement = ref(null)
const fileInput = ref(null)
const dirInput = ref(null)
const formData = ref(null)
const validation = ref({})
const imageError = ref('')
const modalInstance = ref(null)
const fileSelector = ref(null)

const isWindows = computed(() => props.platform === 'windows')
const isNewApp = computed(() => !props.app || props.app.index === -1)

// ---- App Display Profile (server-side per-app display scheme) ----
const displayDevices = ref([])
const globalOutputName = ref('')

const loadDisplayDevices = async () => {
  try {
    const config = await apiJson('/api/config')
    displayDevices.value = Array.isArray(config.display_devices) ? config.display_devices : []
    globalOutputName.value = typeof config.output_name === 'string' ? config.output_name : ''
  } catch (_) {
    displayDevices.value = []
  }
}

const hasForcedDisplayProfile = computed(() => Boolean(formData.value?.['display-target']))
const appDisplayOutput = computed({
  get: () => {
    const target = formData.value?.['display-target']
    if (!target) return '__inherit__'
    if (target === 'virtual') return 'ZakoHDR'
    return formData.value?.['display-output-name'] || '__physical__'
  },
  set: (outputName) => {
    if (outputName === '__inherit__') {
      formData.value['display-target'] = ''
      formData.value['display-output-name'] = ''
    } else if (outputName === 'ZakoHDR') {
      formData.value['display-target'] = 'virtual'
      formData.value['display-output-name'] = ''
    } else {
      formData.value['display-target'] = 'physical'
      formData.value['display-output-name'] = outputName === '__physical__' ? '' : outputName
    }
  },
})
const displayRuleModes = Object.freeze({
  '': 'inherit',
  no_operation: 'no_operation',
  client: 'follow_client',
})
const appResolutionOptionLabels = computed(() => ({
  inherit: t('apps.display_profile_inherit'),
  no_operation: tp('config.resolution_change_no_operation'),
  follow_client: tp('config.resolution_change_automatic'),
}))
const appRefreshRateOptionLabels = computed(() => ({
  inherit: t('apps.display_profile_inherit'),
  follow_client: tp('config.refresh_rate_change_automatic'),
}))
const displayRuleValue = (mode) => {
  if (mode === 'no_operation') return 'no_operation'
  if (mode === 'follow_client') return 'client'
  return ''
}
const hasFixedResolution = computed(() => Boolean(formData.value?.['display-resolution']?.trim()))
const hasFixedRefreshRate = computed(() => Boolean(formData.value?.['display-refresh-rate']?.trim()))
// The fixed-values switch is a real control: it owns the enabled state and
// the inputs are disabled while it is off. It initializes from existing
// values, turns itself on when a value is typed, and clears both values
// when turned off.
const fixedValuesEnabled = ref(false)
watch(fixedValuesEnabled, (enabled) => {
  if (!enabled) {
    formData.value['display-resolution'] = ''
    formData.value['display-refresh-rate'] = ''
  }
})
watch(
  () => [formData.value?.['display-resolution'], formData.value?.['display-refresh-rate']],
  ([resolution, refreshRate]) => {
    if (resolution?.trim() || refreshRate?.trim()) {
      fixedValuesEnabled.value = true
    }
  }
)
const appResolutionRule = computed({
  get: () => {
    if (hasFixedResolution.value) return 'follow_client'
    return displayRuleModes[formData.value?.['display-resolution-mode']] || 'inherit'
  },
  set: (mode) => {
    if (mode !== 'follow_client') {
      formData.value['display-resolution'] = ''
    }
    formData.value['display-resolution-mode'] = displayRuleValue(mode)
  },
})
const appRefreshRateRule = computed({
  get: () => {
    if (hasFixedRefreshRate.value) return 'follow_client'
    return displayRuleModes[formData.value?.['display-refresh-rate-mode']] || 'inherit'
  },
  set: (mode) => {
    if (mode !== 'follow_client') {
      formData.value['display-refresh-rate'] = ''
    }
    formData.value['display-refresh-rate-mode'] = displayRuleValue(mode)
  },
})
const displayProfileValid = computed(() => {
  if (!formData.value?.['display-target']) return true
  if (hasFixedResolution.value && !/^[1-9]\d{1,4}x[1-9]\d{1,4}$/.test(formData.value['display-resolution'])) return false
  if (hasFixedRefreshRate.value && !/^[1-9]\d{0,3}(?:\.\d+)?$/.test(formData.value['display-refresh-rate'])) return false
  return true
})
// The default-physical-display option cannot override a global virtual
// display selection (0-change design): warn the user about it.
const showPhysicalGlobalVddWarning = computed(() =>
  formData.value?.['display-target'] === 'physical' &&
  !formData.value?.['display-output-name'] &&
  globalOutputName.value === 'ZakoHDR'
)
const resHintText = computed(() => {
  const rule = appResolutionRule.value
  if (rule === 'no_operation') return t('apps.display_profile_resolution_ignore_hint')
  if (rule === 'follow_client') return tp('config.resolution_change_ogs_desc')
  return ''
})
const rrHintText = computed(() => {
  const rule = appRefreshRateRule.value
  if (rule === 'follow_client') return t('apps.display_profile_refresh_rate_follow_hint')
  return ''
})
// ---- end App Display Profile ----

const isFormValid = computed(() => {
  // name 字段是必填的，必须验证通过
  const nameValid = validation.value.name?.isValid === true
  
  // cmd 字段不是必填的，如果已验证则使用验证结果，如果未验证或为空则认为有效
  const cmdValid = validation.value.cmd?.isValid !== false  // undefined 或 true 都认为有效
  
  return nameValid && cmdValid && displayProfileValid.value
})

const showMessage = (message, type = 'info') => {
  if (window.showToast) {
    window.showToast(message, type)
  } else if (type === 'error') {
    alert(message)
  } else {
    console.info(message)
  }
}

const initializeModal = () => {
  if (modalInstance.value || !modalElement.value) return

  const Modal = window.bootstrap?.Modal
  if (!Modal) {
    console.warn('Bootstrap Modal not available')
    return
  }

  try {
    modalInstance.value = new Modal(modalElement.value, {
      backdrop: 'static',
      keyboard: false,
    })
  } catch (error) {
    console.warn('Modal initialization failed:', error)
  }
}

const initializeFileSelector = () => {
  const notify = (type) => (message) => showMessage(message, type)
  fileSelector.value = createFileSelector({
    platform: props.platform,
    onSuccess: notify('info'),
    onError: notify('error'),
    onInfo: notify('info'),
  })
}

const ensureDefaultValues = () => {
  const arrayDefaults = ['prep-cmd', 'menu-cmd', 'detached']
  arrayDefaults.forEach((key) => {
    if (!formData.value[key]) formData.value[key] = []
  })

  if (!formData.value['exclude-global-prep-cmd']) {
    formData.value['exclude-global-prep-cmd'] = false
  }
  if (!formData.value['working-dir']) {
    formData.value['working-dir'] = ''
  }

  if (isWindows.value && formData.value.elevated === undefined) {
    formData.value.elevated = false
  }
  if (formData.value['auto-detach'] === undefined) {
    formData.value['auto-detach'] = true
  }
  if (formData.value['wait-all'] === undefined) {
    formData.value['wait-all'] = true
  }
  if (formData.value['exit-timeout'] === undefined) {
    formData.value['exit-timeout'] = 5
  }
  if (isWindows.value && formData.value['mouse-mode'] === undefined) {
    formData.value['mouse-mode'] = 0
  }
  if (isWindows.value && formData.value.gamepad === undefined) {
    formData.value.gamepad = ''
  }
}

const initializeForm = (app) => {
  formData.value = { ...DEFAULT_FORM_DATA, ...deepClone(app) }
  ensureDefaultValues()
  fixedValuesEnabled.value = Boolean(
    formData.value['display-resolution']?.trim() || formData.value['display-refresh-rate']?.trim()
  )
  validation.value = {}
  imageError.value = ''
  // 立即验证所有字段，确保表单状态正确
  nextTick(() => {
    // 验证必填字段 name（总是验证）
    validateField('name')
    // 验证 cmd 字段（如果有值则验证，没有值则标记为有效）
    if (formData.value.cmd && formData.value.cmd.trim()) {
      validateField('cmd')
    } else {
      // cmd 字段不是必填的，如果为空则标记为有效
      validation.value.cmd = { isValid: true, message: '' }
    }
  })
}

const showModal = () => {
  if (!modalInstance.value) initializeModal()
  modalInstance.value?.show()
}

const resetFileSelection = () => {
  fileSelector.value?.resetState()
  fileSelector.value?.cleanupFileInputs(fileInput.value, dirInput.value)
}

const closeModal = () => {
  modalInstance.value?.hide()
  setTimeout(() => {
    resetFileSelection()
    emit('close')
  }, 300)
}

const cleanup = () => {
  modalInstance.value?.dispose()
  resetFileSelection()
}

const validateField = (fieldName) => {
  const validationKey = FIELD_VALIDATION_MAP[fieldName] || fieldName
  const result = validateFieldHelper(validationKey, formData.value[fieldName])
  validation.value[fieldName] = result
  return result
}

// 处理 cmd 字段输入，如果清空则立即更新验证状态
const handleCmdInput = () => {
  // 如果 cmd 字段被清空，立即标记为有效（因为不是必填字段）
  if (!formData.value.cmd || !formData.value.cmd.trim()) {
    validation.value.cmd = { isValid: true, message: '' }
  }
}

const getFieldClass = (fieldName) => {
  const v = validation.value[fieldName]
  if (!v) return ''
  return {
    'is-invalid': !v.isValid,
    'is-valid': v.isValid && formData.value[fieldName],
  }
}

const createCommand = (type) => {
  const baseCmd = type === 'prep' ? { do: '', undo: '' } : { id: nanoid(10), name: '', cmd: '' }
  if (isWindows.value) baseCmd.elevated = false
  return baseCmd
}

const addPrepCommand = () => {
  formData.value['prep-cmd'].push(createCommand('prep'))
}

const removePrepCommand = (index) => {
  formData.value['prep-cmd'].splice(index, 1)
}

const addMenuCommand = () => {
  formData.value['menu-cmd'].push(createCommand('menu'))
}

const removeMenuCommand = (index) => {
  formData.value['menu-cmd'].splice(index, 1)
}

const handlePrepCommandOrderChanged = (newOrder) => {
  formData.value['prep-cmd'] = newOrder
}

const handleMenuCommandOrderChanged = (newOrder) => {
  formData.value['menu-cmd'] = newOrder
}

const testMenuCommand = async (index) => {
  const menuCmd = formData.value['menu-cmd'][index]
  if (!menuCmd.cmd) {
    showMessage(t('apps.test_menu_cmd_empty'), 'error')
    return
  }

  try {
    showMessage(t('apps.test_menu_cmd_executing'))
    const result = await apiPostJson('/api/apps/test-menu-cmd', {
      cmd: menuCmd.cmd,
      working_dir: formData.value['working-dir'] || '',
      elevated: menuCmd.elevated === 'true' || menuCmd.elevated === true,
    })
    const isSuccess = result.status
    showMessage(
      isSuccess
        ? t('apps.test_menu_cmd_success')
        : `${t('apps.test_menu_cmd_failed')}: ${result.error || 'Unknown error'}`,
      isSuccess ? 'info' : 'error'
    )
  } catch (error) {
    showMessage(`${t('apps.test_menu_cmd_failed')}: ${error.message}`, 'error')
  }
}

const addDetachedCommand = () => {
  formData.value.detached.push('')
}

const removeDetachedCommand = (index) => {
  formData.value.detached.splice(index, 1)
}

const handleDetachedCommandOrderChanged = (newOrder) => {
  formData.value.detached = newOrder
}

const updateImage = (imagePath) => {
  formData.value['image-path'] = imagePath
  imageError.value = ''
}

const handleImageError = (error) => {
  imageError.value = error
}

const onFilePathSelected = (fieldName, filePath) => {
  formData.value[fieldName] = filePath
  validateField(fieldName)
}

const selectFile = (fieldName) => {
  if (!fileSelector.value) {
    showMessage(t('apps.file_selector_not_initialized'), 'error')
    return
  }
  fileSelector.value.selectFile(fieldName, fileInput.value, onFilePathSelected)
}

const selectDirectory = (fieldName) => {
  if (!fileSelector.value) {
    showMessage(t('apps.file_selector_not_initialized'), 'error')
    return
  }
  fileSelector.value.selectDirectory(fieldName, dirInput.value, onFilePathSelected)
}

const getPlaceholderText = (fieldName) => fileSelector.value?.getPlaceholderText(fieldName) || ''

const getButtonTitle = (type) => fileSelector.value?.getButtonTitle(type) || t('apps.select')

const saveApp = async () => {
  const formValidation = validateAppForm(formData.value)
  if (!formValidation.isValid) {
    if (formValidation.errors.length) alert(formValidation.errors[0])
    return
  }

  const editedApp = normalizeAppDisplayProfile({ ...formData.value })
  if (editedApp['image-path']) {
    editedApp['image-path'] = editedApp['image-path'].toString().replace(/"/g, '')
  }

  emit('save-app', editedApp)
}

watch(
  () => props.app,
  (newApp) => {
    if (newApp) {
      initializeForm(newApp)
      nextTick(showModal)
    }
  },
  { immediate: true }
)

onMounted(() => {
  void loadDisplayDevices()
  nextTick(() => {
    initializeModal()
    initializeFileSelector()
  })
})

onBeforeUnmount(cleanup)
</script>

<style lang="less" scoped>
.modal-body {
  max-height: calc(100vh - 200px);
  overflow-y: auto;

  /* 滚动条美化 */
  &::-webkit-scrollbar {
    width: 6px;
  }

  &::-webkit-scrollbar-track {
    background: transparent;
    border-radius: 3px;
  }

  &::-webkit-scrollbar-thumb {
    background: var(--ui-border-strong);
    border-radius: 3px;
    transition: background 0.2s ease;

    &:hover {
      background: var(--ui-accent);
    }
  }
}

.modal-footer-enhanced {
  border-top: 1px solid var(--ui-border);
  padding: 1rem 1.5rem;
  display: flex;
  justify-content: space-between;
  align-items: center;
  
  background: var(--ui-surface);
}

.save-status {
  font-size: 0.875rem;
  color: var(--ui-text-muted);
}

.is-invalid {
  border-color: var(--ui-danger);
}

.is-valid {
  border-color: var(--ui-success);
}

.monospace {
  font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
}

.cmd-examples {
  margin-top: 0.5rem;
  padding: 0.75rem;
  border-radius: var(--ui-radius-md);
  background: var(--ui-surface);
  border: 1px solid var(--ui-border);

  &-header {
    font-size: 0.75rem;
    font-weight: 600;
    margin-bottom: 0.5rem;
    
    color: var(--ui-accent);
  }

  &-tags {
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem;
    line-height: 1.5;
  }
}

.cmd-tag {
  display: inline-flex;
  align-items: center;
  gap: 0.375rem;
  padding: 0.375rem 0.625rem;
  border-radius: var(--ui-radius-sm);
  font-size: 0.75rem;
  transition: all 0.2s ease;
  
  background: var(--ui-surface-strong);
  border: 1px solid var(--ui-border);

  &:hover {
    transform: translateY(-1px);
    background: var(--ui-surface-hover);
    border-color: var(--ui-border-strong);
    box-shadow: var(--ui-shadow-sm);
  }

  code {
    font-family: monospace;
    font-size: 0.7rem;
    padding: 0.125rem 0.5rem;
    border-radius: 5px;
    border: none;
    
    background: var(--ui-accent-soft);
    color: var(--ui-accent);
  }

  &-desc {
    font-size: 0.7rem;
    white-space: nowrap;
    
    color: var(--ui-text-muted);
  }
}

/* ===== App Display Profile ===== */
.display-rule-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(min(100%, 13rem), 1fr));
  gap: 0.75rem;
  margin-bottom: 1rem;
}

.display-options-note {
  margin: 0 0 1rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-sm);
  background: var(--ui-accent-soft);
  color: var(--ui-text-secondary);
}

.display-options-note summary {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.7rem 0.85rem;
  color: var(--ui-text-primary);
  font-size: 0.85rem;
  font-weight: 600;
  cursor: pointer;
  list-style: none;
}

.display-options-note summary::-webkit-details-marker {
  display: none;
}

.display-options-note summary i {
  color: var(--ui-accent);
}

.display-options-note summary::after {
  margin-left: auto;
  font-family: 'Font Awesome 7 Free';
  font-weight: 900;
  content: '\f078';
  transition: transform 0.2s ease;
}

.display-options-note[open] summary::after {
  transform: rotate(180deg);
}

.display-options-note p {
  margin: 0;
  padding: 0 0.85rem 0.85rem;
  font-size: 0.82rem;
  line-height: 1.5;
}

.display-profile-alert {
  border-color: color-mix(in srgb, var(--bs-warning) 45%, var(--ui-border));
  background: color-mix(in srgb, var(--bs-warning) 12%, var(--ui-surface));
  color: var(--ui-text);
}

/* Advanced options (server-side fixed values) */
.advanced-options-note {
  margin-top: 0.4rem;
  border: 1px dashed var(--ui-accent);
  border-radius: var(--ui-radius-md);
  background: color-mix(in srgb, var(--ui-accent) 4%, var(--ui-surface));
}

.advanced-options-note summary {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.7rem 0.85rem;
  color: var(--ui-accent);
  font-size: 0.85rem;
  font-weight: 650;
  cursor: pointer;
  list-style: none;
}

.advanced-options-note summary::-webkit-details-marker {
  display: none;
}

.advanced-options-note summary::after {
  margin-left: auto;
  font-family: 'Font Awesome 7 Free';
  font-weight: 900;
  content: '\f078';
  color: var(--ui-text-secondary);
  transition: transform 0.2s ease;
}

.advanced-options-note[open] summary::after {
  transform: rotate(180deg);
}

.advanced-options-note .advanced-sub {
  color: var(--ui-text-muted);
  font-weight: 400;
  font-size: 0.75rem;
}

.advanced-body {
  padding: 0 0.9rem 0.9rem;
}

.advanced-body .field-row {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  margin: 0.45rem 0;
  flex-wrap: wrap;
}

.advanced-body .field-row input[type="text"] {
  width: 9rem;
  font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
  font-size: 0.8rem;
}

.advanced-body .field-row .field-sep {
  color: var(--ui-text-secondary);
  font-size: 0.8rem;
}

.advanced-body .field-row.hdr-row .form-select {
  width: 14rem;
}

.advanced-body .impl-note {
  margin-top: 0.5rem;
  padding: 0.55rem 0.7rem;
  border-radius: var(--ui-radius-sm);
  background: #fff8e6;
  border: 1px solid #f0d99a;
  color: #6b5310;
  font-size: 0.75rem;
  line-height: 1.5;
}

@media (max-width: 767.98px) {
  .display-rule-grid {
    grid-template-columns: minmax(0, 1fr);
  }
}
</style>
