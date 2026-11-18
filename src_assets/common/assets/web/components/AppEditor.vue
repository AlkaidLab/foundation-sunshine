<template>
  <MPage wide class="app-page" :title="pageTitle" back :back-label="t('apps.applications_title')" @back="requestClose">
    <input ref="fileInput" type="file" class="visually-hidden" tabindex="-1" aria-hidden="true" />
    <input ref="dirInput" type="file" class="visually-hidden" tabindex="-1" aria-hidden="true" webkitdirectory />

    <form v-if="formData" class="apps-editor apps-editor--page" @submit.prevent="saveApp">
      <!-- the hero: the poster of the application (the cover), editable below under 封面 -->
      <div class="apps-hero" :class="{ 'is-empty': !heroUrl }" :style="heroUrl ? { backgroundImage: `url(${heroUrl})` } : undefined">
        <MIcon v-if="!heroUrl" class="apps-hero__glyph" name="image" />
        <span class="apps-hero__fill" />
        <MButton class="apps-hero__edit" variant="tonal" size="sm" icon="image" @click="toCover">{{ t('apps.change_cover') }}</MButton>
      </div>

      <!-- the program that was chosen is one of a game that has its own way to be added: say so, and offer it (never converted by itself) -->
      <MBanner v-if="gameHint" class="apps-gamehint" tone="info" icon="sports_esports">
        {{ gameHint.text }}
        <template #actions><MButton variant="text" size="sm" @click="emit('to-game', gameHint.path)">{{ spxCopy.gameHintAction }}</MButton></template>
      </MBanner>

      <!-- name, command, run as administrator -->
      <MSection>
        <MGroup>
          <MFieldItem v-model="formData.name" :label="t('apps.app_name')" :error="errorOf('name')" required />
          <div class="m-item m-item--field apps-pathrow">
            <MField v-model="formData.cmd" :label="t('apps.cmd')" :supporting="t('apps.cmd_desc')" :error="errorOf('cmd')" :placeholder="getPlaceholderText('cmd')" />
            <MIconButton icon="folder_open" :label="getButtonTitle('file')" @click="selectFile('cmd')" />
          </div>
          <div class="m-item apps-examples">
            <span class="apps-examples__head">{{ t('apps.cmd_examples_title') }}<MInfo :text="t('apps.cmd_note')" /></span>
            <span class="apps-examples__tags">
              <span v-for="example in COMMAND_EXAMPLES" :key="example.code" class="cmd-tag"><code>{{ example.code }}</code><span class="cmd-tag-desc">{{ example.label || t('apps.launch_program_directly') }}</span></span>
            </span>
          </div>
          <MSwitchItem v-if="isWindows" :model-value="isOn(formData.elevated)" :title="t('_common.run_as')" :text="t('apps.run_as_desc')" @update:model-value="(v) => setFlag('elevated', v)" />
          <MChoiceItem v-model="formData['fs-kind']" :title="t('apps.fs_kind')" :options="fsKindOptions" :cancel-label="t('_common.cancel')" />
        </MGroup>
      </MSection>

      <!-- the cover -->
      <MSection id="apps-cover" :title="t('apps.image_settings')">
        <div class="cover-pair">
        <ImageSelector kind="poster" :title="t('apps.cover_portrait')" :image-path="formData['image-path']" :app-name="formData.name" @update-image="updateImage" @image-error="handleImageError" />
        <ImageSelector kind="hero" :title="t('apps.cover_landscape')" :image-path="formData.hero" :app-name="formData.name" @update-image="updateHero" @image-error="handleImageError" />
        </div>
      </MSection>

      <!-- launch -->
      <MSection :title="t('apps.section_launch')">
        <MGroup>
          <div class="m-item m-item--field apps-pathrow">
            <MField v-model="formData['working-dir']" :label="t('apps.working_dir')" :supporting="t('apps.working_dir_desc')" :error="errorOf('working-dir')" :placeholder="getPlaceholderText('working-dir')" />
            <MIconButton icon="folder_open" :label="getButtonTitle('directory')" @click="selectDirectory('working-dir')" />
          </div>
          <MFieldItem v-model="formData.output" :label="t('apps.output_name')" :supporting="t('apps.output_desc')" :error="errorOf('output')" />
        </MGroup>
      </MSection>
      <MSection :title="t('apps.behaviour')">
        <MGroup>
          <MSwitchItem :model-value="isOn(formData['auto-detach'])" :title="t('apps.auto_detach')" :text="t('apps.auto_detach_desc')" @update:model-value="(v) => setFlag('auto-detach', v)" />
          <MSwitchItem :model-value="isOn(formData['wait-all'])" :title="t('apps.wait_all')" :text="t('apps.wait_all_desc')" @update:model-value="(v) => setFlag('wait-all', v)" />
          <MFieldItem v-model="formData['exit-timeout']" type="number" min="0" :label="t('apps.exit_timeout')" :supporting="t('apps.exit_timeout_desc')" :error="errorOf('exit-timeout')" />
        </MGroup>
      </MSection>

      <!-- prepare commands: before the stream, and the menu of the client -->
      <MSection :title="t('apps.command_settings')">
        <MGroup>
          <MSwitchItem :model-value="isOn(formData['exclude-global-prep-cmd'])" :title="t('apps.global_prep_name')" :text="t('apps.global_prep_desc')" @update:model-value="(v) => setFlag('exclude-global-prep-cmd', v)" />
        </MGroup>
      </MSection>
      <MSection :title="t('apps.cmd_prep_name')">
        <template #action><MInfo :text="t('apps.cmd_prep_desc')" /></template>
        <CommandTable :commands="formData['prep-cmd']" :platform="platform" type="prep" @add-command="addPrepCommand" @remove-command="removePrepCommand" @order-changed="handlePrepCommandOrderChanged" />
      </MSection>
      <MSection :title="t('apps.menu_cmd_name')">
        <template #action><MInfo :text="t('apps.menu_cmd_desc')" /></template>
        <CommandTable :commands="formData['menu-cmd']" :platform="platform" type="menu" @add-command="addMenuCommand" @remove-command="removeMenuCommand" @test-command="testMenuCommand" @order-changed="handleMenuCommandOrderChanged" />
      </MSection>

      <!-- advanced (expert): shown after the person confirms, or at once when the application already uses one of them -->
      <div v-if="!showAdvanced" class="apps-gate">
        <MIcon name="tune" />
        <h3>{{ t('apps.advanced_gate_title') }}</h3>
        <p>{{ t('apps.advanced_gate_body') }}</p>
        <MButton variant="tonal" icon="lock_open" @click="confirmAdvanced = true">{{ t('apps.advanced_gate_show') }}</MButton>
      </div>
      <template v-else>
        <MSection v-if="isWindows" :title="t('apps.advanced_options')">
          <MGroup>
            <MChoiceItem v-if="isWindows" v-model="formData.gamepad" :title="t('apps.gamepad_mode')" :options="gamepadOptions" :cancel-label="t('_common.cancel')" />
            <MChoiceItem v-if="isWindows" v-model="formData['mouse-mode']" :title="t('apps.mouse_mode')" :options="mouseOptions" :cancel-label="t('_common.cancel')" />
          </MGroup>
          <MBanner v-if="isWindows && formData.gamepad === 'ds5'" tone="info" icon="info">{{ t('config.gamepad_ds5_hint_windows') }}</MBanner>
          <MFooter v-if="isWindows">{{ t('apps.gamepad_mode_desc') }}</MFooter>
          <MFooter v-if="isWindows">{{ t('apps.mouse_mode_desc') }}</MFooter>
        </MSection>
        <MSection :title="t('apps.detached_cmds')">
          <template #action><MInfo :text="`${t('apps.detached_cmds_desc')} ${t('_common.note')} ${t('apps.detached_cmds_note')}`" /></template>
          <CommandTable :commands="formData.detached" :platform="platform" type="detached" @add-command="addDetachedCommand" @remove-command="removeDetachedCommand" @order-changed="handleDetachedCommandOrderChanged" />
        </MSection>
        <MSection v-if="isWindows" :title="t('apps.rtx_hdr')">
          <MGroup>
            <MChoiceItem v-model="formData['rtx-hdr'].mode" :title="t('apps.rtx_hdr_mode')" :options="modeOptions" :cancel-label="t('_common.cancel')" />
            <template v-if="formData['rtx-hdr'].mode === 'on'">
              <MFieldItem v-model="formData['rtx-hdr'].contrast" type="number" :label="t('apps.rtx_hdr_contrast')" min="-100" max="100" />
              <MFieldItem v-model="formData['rtx-hdr'].saturation" type="number" :label="t('apps.rtx_hdr_saturation')" min="-100" max="100" />
              <MFieldItem v-model="formData['rtx-hdr']['middle-gray']" type="number" :label="t('apps.rtx_hdr_middle_gray')" min="10" max="100" />
              <MFieldItem v-model="formData['rtx-hdr']['peak-nits']" type="number" :label="t('apps.rtx_hdr_peak_nits')" min="400" max="1000" step="50" />
            </template>
          </MGroup>
          <MFooter>{{ t('apps.rtx_hdr_desc') }}</MFooter>
        </MSection>
        <MSection v-if="isWindows" title="DLSS NR">
          <MGroup>
            <MChoiceItem v-model="formData.dlssnr.mode" :title="t('apps.dlssnr_mode')" :options="modeOptions" :cancel-label="t('_common.cancel')" />
            <template v-if="formData.dlssnr.mode === 'on'">
              <MFieldItem v-model="formData.dlssnr.intensity" type="number" :label="t('apps.dlssnr_intensity')" min="0" max="1" step="0.05" />
              <MFieldItem v-model="formData.dlssnr.style" type="number" :label="t('apps.dlssnr_style')" min="0" max="4" step="1" />
              <MSwitchItem :model-value="isOn(formData.dlssnr['ui-correction'])" :title="t('apps.dlssnr_ui')" @update:model-value="(v) => (formData.dlssnr['ui-correction'] = v ? 'true' : 'false')" />
              <MChoiceItem v-model="formData.dlssnr['motion-quality']" :title="t('apps.dlssnr_motion')" :options="motionOptions" :cancel-label="t('_common.cancel')" />
            </template>
          </MGroup>
          <MFooter>{{ t('apps.dlssnr_desc') }}</MFooter>
        </MSection>
      </template>

      <!-- the save bar of the page -->
      <div class="apps-savebar" role="toolbar">
        <span class="apps-status" :class="{ 'is-bad': !isFormValid || !!imageError }" role="status">
          <MIcon :name="isFormValid && !imageError ? 'check_circle' : 'warning'" fill />{{ imageError || (isFormValid ? t('apps.form_valid') : t('apps.form_invalid')) }}
        </span>
        <span class="apps-savebar__fill" />
        <MButton variant="text" @click="requestClose">{{ t('_common.cancel') }}</MButton>
        <MButton icon="save" :loading="disabled" :disabled="!isFormValid || unchanged" @click="saveApp">{{ t('_common.save') }}</MButton>
      </div>
    </form>
  </MPage>

  <MDialog v-model="confirmAdvanced" :title="t('apps.advanced_gate_confirm_title')" icon="warning">
    <p>{{ t('apps.advanced_gate_confirm_body') }}</p>
    <template #actions>
      <MButton variant="text" @click="confirmAdvanced = false">{{ t('_common.cancel') }}</MButton>
      <MButton @click="advancedConfirmed = true; confirmAdvanced = false">{{ t('apps.advanced_gate_confirm') }}</MButton>
    </template>
  </MDialog>

  <MDialog v-model="confirmDiscard" :title="t('apps.discard_title')" icon="warning">
    <p>{{ t('apps.discard_body') }}</p>
    <template #actions>
      <MButton variant="text" @click="confirmDiscard = false">{{ t('apps.keep_editing') }}</MButton>
      <MButton variant="danger" @click="closeModal">{{ t('apps.discard') }}</MButton>
    </template>
  </MDialog>
</template>

<script setup>
import { useNotify } from 'foundation-ui/notify'
import { ref, computed, watch, onMounted, onBeforeUnmount, nextTick } from 'vue'
import { useI18n } from 'vue-i18n'
import { validateField as validateFieldHelper, validateAppForm } from '../utils/validation.js'
import { nanoid } from 'nanoid'
import CommandTable from './CommandTable.vue'
import ImageSelector from './ImageSelector.vue'
import { MBanner, MButton, MChip, MChoiceItem, MDialog, MField, MFieldItem, MFooter, MGroup, MIcon, MIconButton, MInfo, MPage, MSection, MSwitchItem } from 'foundation-ui/vue'
import { createFileSelector } from '../utils/fileSelection.js'
import { apiPostJson } from '../utils/apiFetch.js'
import { deepClone } from '../utils/helpers.js'
import { PER_APP_GAMEPAD_MODES } from '../utils/gamepadModes.js'
import { normalizeDlssnrConfig } from '../utils/dlssnr.js'
import { useSparxie } from '../composables/useSparxie.js'
import { useSparxieCopy } from '../composables/useSparxieCopy.js'
import { DEFAULT_CATALOG } from '../utils/sparxie/model.js'
import { getImagePreviewUrl } from '../utils/imageUtils.js'

const DEFAULT_FORM_DATA = Object.freeze({
  dlssnr: normalizeDlssnrConfig(),
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
  hero: '',
  'fs-kind': '',
  'working-dir': '',
  'rtx-hdr': {
    mode: 'inherit',
    contrast: 0,
    saturation: 0,
    'middle-gray': 50,
    'peak-nits': 1000,
  },
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

const emit = defineEmits(['close', 'save-app', 'to-game'])

const { t, locale } = useI18n()

const open = ref(true)
const confirmDiscard = ref(false)
const initialSnapshot = ref('')
const fileInput = ref(null)
const dirInput = ref(null)
const formData = ref(null)
const validation = ref({})
const imageError = ref('')
const fileSelector = ref(null)

const isWindows = computed(() => props.platform === 'windows')

// the editor is four sections; the advanced one (expert options) opens after a confirmation, or at once when the application already uses one of them
const section = ref('basic')
const confirmAdvanced = ref(false)
const advancedConfirmed = ref(false)
const usesAdvanced = computed(() => {
  const f = formData.value
  if (!f) return false
  return !!(f.gamepad || Number(f['mouse-mode']) || (f.detached && f.detached.length) || (f['rtx-hdr'] && f['rtx-hdr'].mode && f['rtx-hdr'].mode !== 'inherit') || (f.dlssnr && f.dlssnr.mode && f.dlssnr.mode !== 'inherit'))
})
const showAdvanced = computed(() => advancedConfirmed.value || usesAdvanced.value)
// a section with a field that does not validate shows an error icon on its tab
// A program that belongs to a game the page 添加游戏 knows (its executable is on the host's list) is offered that way: a banner, one tap to take the path over. The host names the game and the region.
const spx = useSparxie()
const spxCopy = useSparxieCopy()
const exeOfCmd = (cmd) => { const m = /^\s*"([^"]+\.exe)"/i.exec(String(cmd || '')) || /^\s*(\S+\.exe)\b/i.exec(String(cmd || '')); return m ? m[1] : '' }
const hint = ref(null)
let hintSeq = 0
watch(() => formData.value?.cmd, async (cmd) => {
  const exe = exeOfCmd(cmd)
  const mine = ++hintSeq
  if (!exe || !spx.available.value || !isNewApp.value) { hint.value = null; return }
  const file = exe.split(/[\\/]/).pop().toLowerCase()
  try {
    const { catalog } = await spx.client().games()
    const title = Object.entries(catalog).find(([id, row]) => (row.exes.length ? row.exes : DEFAULT_CATALOG[id]?.exes || []).some((n) => n.toLowerCase() === file))
    if (!title) { if (mine === hintSeq) hint.value = null; return }
    const found = await spx.client().identify(exe)
    if (mine !== hintSeq) return
    hint.value = found.install ? { path: exe, game: found.install.game, region: found.install.region, name: (title[1].name && /^zh/i.test(locale.value) ? title[1].name : title[1].nameEn) || title[1].name || title[0] } : null
  } catch { if (mine === hintSeq) hint.value = null }
})
const gameHint = computed(() => (hint.value ? { path: hint.value.path, text: spxCopy.value.gameHint(hint.value.name, hint.value.region ? spxCopy.value.regions[hint.value.region] : '') } : null))
// the hero shows the poster of the application (its own image)
const heroUrl = computed(() => {
  const img = formData.value?.hero || formData.value?.['image-path']
  return img ? getImagePreviewUrl(String(img).replace(/"/g, '')) : ''
})
const pageTitle = computed(() => (isNewApp.value ? t('apps.add_new') : formData.value?.name || t('apps.edit')))
const toCover = () => document.getElementById('apps-cover')?.scrollIntoView({ behavior: 'smooth', block: 'start' })
const isNewApp = computed(() => !props.app || props.app.index === -1)
const isFormValid = computed(() => {
  // name 字段是必填的，必须验证通过
  const nameValid = validation.value.name?.isValid === true
  
  // cmd 字段不是必填的，如果已验证则使用验证结果，如果未验证或为空则认为有效
  const cmdValid = validation.value.cmd?.isValid !== false  // undefined 或 true 都认为有效
  
  return nameValid && cmdValid
})

const showMessage = (message, type = 'info') => {
  if (window.showToast) {
    window.showToast(message, type)
  } else if (type === 'error') {
    useNotify().snack(message)
  } else {
    console.info(message)
  }
}

const initializeFileSelector = () => {
  const notify = (type) => (message) => showMessage(message, type)
  fileSelector.value = createFileSelector({
    platform: props.platform,
    translate: t,
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
  if (formData.value['fs-kind'] === undefined) {
    formData.value['fs-kind'] = ''
  }
  if (formData.value.hero === undefined) {
    formData.value.hero = ''
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
  formData.value.dlssnr = normalizeDlssnrConfig(formData.value.dlssnr)
  const rtxHdr = formData.value['rtx-hdr']
  formData.value['rtx-hdr'] = {
    ...DEFAULT_FORM_DATA['rtx-hdr'],
    ...(rtxHdr && typeof rtxHdr === 'object' ? rtxHdr : {}),
  }
}

const initializeForm = (app) => {
  formData.value = { ...DEFAULT_FORM_DATA, ...deepClone(app) }
  ensureDefaultValues()
  validation.value = {}
  imageError.value = ''
  initialSnapshot.value = JSON.stringify(formData.value)
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

const resetFileSelection = () => {
  fileSelector.value?.resetState()
  fileSelector.value?.cleanupFileInputs(fileInput.value, dirInput.value)
}

const isDirty = () => !!formData.value && JSON.stringify(formData.value) !== initialSnapshot.value
// 保存 of an existing application waits for a change (a new application can always be saved); it is off again when the change is undone
const unchanged = computed(() => !isNewApp.value && !!formData.value && JSON.stringify(formData.value) === initialSnapshot.value)

// Cancel, Escape or the scrim: ask before throwing away edits
const requestClose = () => {
  if (isDirty()) confirmDiscard.value = true
  else closeModal()
}

const closeModal = () => {
  confirmDiscard.value = false
  open.value = false
  resetFileSelection()
  emit('close')
}

const cleanup = () => {
  resetFileSelection()
}

const validateField = (fieldName) => {
  const validationKey = FIELD_VALIDATION_MAP[fieldName] || fieldName
  const result = validateFieldHelper(validationKey, formData.value[fieldName], {}, t)
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

const errorOf = (fieldName) => {
  const v = validation.value[fieldName]
  return v && v.isValid === false ? v.message || t('apps.form_invalid') : ''
}

const isOn = (value) => value === true || value === 'true'
const setFlag = (key, on) => { formData.value[key] = on ? 'true' : 'false' }

const COMMAND_EXAMPLES = [
  { code: 'cmd /c "start xbox:"', label: 'Xbox Game' },
  { code: 'steam://open/bigpicture', label: 'Steam Big Picture' },
  { code: 'cmd /c "start ms-gamebar:"', label: 'Xbox Game Bar' },
  { code: 'cmd /c "start playnite://playnite/showMainWindow"', label: 'Playnite' },
  { code: '"C:\\Program Files\\...\\game.exe"', label: '' },
]
const modeOptions = computed(() => [
  { value: 'inherit', label: t('apps.rtx_hdr_inherit') },
  { value: 'on', label: t('apps.rtx_hdr_on') },
  { value: 'off', label: t('apps.rtx_hdr_off') },
])
const motionOptions = computed(() => [
  { value: 0, label: t('apps.dlssnr_motion_off'), text: t('apps.dlssnr_motion_hint') },
  { value: 1, label: t('apps.dlssnr_motion_fast') },
  { value: 2, label: t('apps.dlssnr_motion_balanced') },
  { value: 3, label: t('apps.dlssnr_motion_quality') },
])
const gamepadOptions = computed(() => PER_APP_GAMEPAD_MODES.map((mode) => ({ value: mode.value, label: t(mode.labelKey) })))
const mouseOptions = computed(() => [
  { value: 0, label: t('apps.mouse_mode_auto') },
  { value: 1, label: t('apps.mouse_mode_vmouse') },
  { value: 2, label: t('apps.mouse_mode_sendinput') },
])
const fsKindOptions = computed(() => [
  { value: '', label: t('apps.fs_kind_auto') },
  { value: 'bigscreen', label: t('apps.fs_kind_bigscreen') },
  { value: 'desktop', label: t('apps.fs_kind_desktop') },
  { value: 'game', label: t('apps.fs_kind_game') },
  { value: 'tool', label: t('apps.fs_kind_tool') },
])

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

// the landscape cover: either cover may be set, both, or none
const updateHero = (imagePath) => {
  formData.value.hero = imagePath
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
  formData.value.dlssnr = normalizeDlssnrConfig(formData.value.dlssnr)
  const formValidation = validateAppForm(formData.value, t)
  if (!formValidation.isValid) {
    if (formValidation.errors.length) useNotify().snack(formValidation.errors[0])
    return
  }

  const editedApp = { ...formData.value }
  if (editedApp['image-path']) {
    editedApp['image-path'] = editedApp['image-path'].toString().replace(/"/g, '')
  }
  if (!editedApp['fs-kind']) delete editedApp['fs-kind']
  if (editedApp.hero) editedApp.hero = editedApp.hero.toString().replace(/"/g, '')
  else delete editedApp.hero

  emit('save-app', editedApp)
}

watch(
  () => props.app,
  (newApp) => {
    if (newApp) {
      initializeForm(newApp)
      open.value = true
    }
  },
  { immediate: true }
)

onMounted(() => {
  window.scrollTo(0, 0)
  nextTick(initializeFileSelector)
})

// Live validation while typing (the fields have no blur event)
watch(() => formData.value?.name, () => formData.value && validateField('name'))
watch(() => formData.value?.output, () => formData.value && validateField('output'))
watch(() => formData.value?.['working-dir'], () => formData.value && validateField('working-dir'))
watch(() => formData.value?.['exit-timeout'], () => formData.value && validateField('exit-timeout'))
watch(() => formData.value?.cmd, () => {
  if (!formData.value) return
  if (formData.value.cmd && formData.value.cmd.trim()) validateField('cmd')
  else handleCmdInput()
})

onBeforeUnmount(cleanup)
</script>


<style lang="less" scoped>
.visually-hidden {
  position: absolute;
  width: 1px;
  height: 1px;
  overflow: hidden;
  clip: rect(0 0 0 0);
}

// the page of an application: the hero (the poster), the groups, the save bar at the bottom of the page
.apps-hero {
  position: relative;
  min-height: 190px;
  margin-bottom: 8px;
  padding: 16px;
  display: flex;
  align-items: flex-end;
  justify-content: space-between;
  gap: 12px;
  overflow: hidden;
  border-radius: 28px;
  background: linear-gradient(135deg, var(--md-sys-color-primary-container), var(--md-sys-color-tertiary-container)) center / cover;
  background-size: cover;
  background-position: center 22%;
}
.apps-hero.is-empty { align-items: flex-end; }
.apps-hero__glyph { position: absolute; left: 50%; top: 50%; translate: -50% -50%; font-size: 56px; opacity: 0.55; color: var(--md-sys-color-on-primary-container); }
.apps-hero__chips { display: flex; flex-wrap: wrap; gap: 8px; }
.apps-hero__fill { flex: 1; }
.apps-savebar {
  position: sticky;
  bottom: 12px;
  z-index: 6;
  display: flex;
  align-items: center;
  gap: 8px;
  margin-top: 16px;
  padding: 10px 12px 10px 20px;
  border-radius: 28px;
  background: var(--md-sys-color-surface-container-high);
  box-shadow: 0 2px 10px rgb(0 0 0 / 0.18);
}
.apps-savebar__fill { flex: 1; }

// motion: the page rises with the default spatial spring when it opens, the groups one after another (capped, so a long page does not make the last one wait); the groups of the advanced part,
// which come into the DOM when the person confirms, pop in the same way. The reduced-motion setting shortens all of it (foundation-ui motion.css).
@keyframes apps-rise {
  from { opacity: 0; transform: translateY(16px) scale(0.985); }
}
.apps-editor--page > * {
  animation: apps-rise var(--m-spring-default-spatial-duration, 400ms) var(--m-spring-default-spatial, ease-out) backwards;
}
.apps-editor--page > :nth-child(2) { animation-delay: 35ms; }
.apps-editor--page > :nth-child(3) { animation-delay: 70ms; }
.apps-editor--page > :nth-child(4) { animation-delay: 105ms; }
.apps-editor--page > :nth-child(5) { animation-delay: 140ms; }
.apps-editor--page > :nth-child(6) { animation-delay: 175ms; }
.apps-editor--page > :nth-child(n + 7) { animation-delay: 210ms; }
</style>
