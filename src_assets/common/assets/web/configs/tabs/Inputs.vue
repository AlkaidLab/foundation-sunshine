<script setup>
import { useNotify } from 'foundation-ui/notify'
import { askConfirm } from '../../composables/askDialog.js'
import { reactive, computed, onMounted, onBeforeUnmount, toRef } from 'vue'
import { useI18n } from 'vue-i18n'
import { MChip, MControllerArt, MFooter, MGroup, MIconButton, MItem, MSection } from 'foundation-ui/vue'
import { isChanged } from 'foundation-ui/settings/index.js'
import CfgCards from '../../components/cfg/CfgCards.vue'
import CfgChoice from '../../components/cfg/CfgChoice.vue'
import CfgExpert from '../../components/cfg/CfgExpert.vue'
import CfgField from '../../components/cfg/CfgField.vue'
import CfgRow from '../../components/cfg/CfgRow.vue'
import CfgSub from '../../components/cfg/CfgSub.vue'
import CfgSwitch from '../../components/cfg/CfgSwitch.vue'
import GamepadPerGame from './input/GamepadPerGame.vue'
import DualSenseGamepad from '../../components/dualsense/DualSenseGamepad.vue'
import DualSenseSubs from '../../components/dualsense/DualSenseSubs.vue'
import { useRuntimeSessions } from '../../composables/useRuntimeSessions.js'
import { ds4HintOf } from '../../utils/gamepadHint.js'

const { t } = useI18n()

const props = defineProps({
  platform: {
    type: String,
    required: true,
  },
  config: {
    type: Object,
    required: true,
  },
})

const config = toRef(props, 'config')

/**
 * 创建一个驱动管理器，统一封装状态/加载/操作逻辑，避免 vmouse / vigem 的重复代码。
 * @param {string} driverKey - window 上挂载的驱动对象属性名（如 'vmouseDriver'）
 * @param {object} initialStatus - 初始状态对象
 */
function createDriverManager(driverKey, initialStatus) {
  let refreshTimeoutId = null

  const driver = () => window[driverKey]

  function dispose() {
    if (refreshTimeoutId !== null) {
      clearTimeout(refreshTimeoutId)
      refreshTimeoutId = null
    }
  }

  const manager = reactive({
    available: false,
    status: { ...initialStatus },
    loading: false,
    operating: false,

    get stateIcon() {
      if (manager.status.running) return 'check_circle'
      if (manager.status.installed) return 'warning'
      return 'cancel'
    },

    async refresh() {
      manager.loading = true
      try {
        const api = driver()
        if (!api) return
        manager.status = await api.getStatus()
      } catch {
        /* ignore */
      } finally {
        manager.loading = false
      }
    },

    async runOp(confirmKey, op) {
      // removing a driver is destructive: the error-colour button with the verb; installing keeps the primary one
      const removing = /uninstall/.test(confirmKey)
      if (!(await askConfirm(t(confirmKey), removing ? { confirmText: t('config.vmouse_uninstall'), danger: true } : {}))) return

      const api = driver()
      if (!api) return

      manager.operating = true
      try {
        await op(api)
        dispose()
        refreshTimeoutId = window.setTimeout(() => {
          refreshTimeoutId = null
          manager.refresh()
        }, 2000)
      } catch (e) {
        useNotify().snack(e instanceof Error ? e.message : String(e))
      } finally {
        manager.operating = false
      }
    },

    dispose,
  })

  return manager
}

// vmouse 驱动管理
const vmouse = createDriverManager('vmouseDriver', { installed: false, running: false, status_text: '' })
const installVmouse = () => vmouse.runOp('config.vmouse_confirm_install', d => d.install())
const uninstallVmouse = () => vmouse.runOp('config.vmouse_confirm_uninstall', d => d.uninstall())

const vmouseStatusLabel = computed(() => {
  if (vmouse.loading) return t('config.vmouse_status_checking')
  if (vmouse.status.running) return t('config.vmouse_status_running')
  if (vmouse.status.installed) return t('config.vmouse_status_installed')
  return t('config.vmouse_status_not_installed')
})

// ViGEmBus 虚拟手柄驱动管理
const vigem = createDriverManager('vigemDriver',
  { installed: false, running: false, version: '', version_ok: false, status_text: '' })

// a client streaming right now whose PlayStation pad runs as a DualShock 4: one line with the reason and what to do
const runtimeSessions = useRuntimeSessions()
const ds4Hint = computed(() => ds4HintOf(runtimeSessions.sessions.value, String(config.value.gamepad ?? 'auto')))
const installVigem = (force = false) => vigem.runOp(
  force ? 'config.vigem_confirm_reinstall' : 'config.vigem_confirm_install',
  d => d.install(force)
)
const uninstallVigem = () => vigem.runOp('config.vigem_confirm_uninstall', d => d.uninstall())

const vigemStatusLabel = computed(() => {
  if (vigem.loading) return t('config.vigem_status_checking')

  const s = vigem.status
  if (!s.installed) return t('config.vigem_status_not_installed')
  if (!s.version_ok) return t('config.vigem_status_outdated')
  if (s.running) {
    const running = t('config.vigem_status_running')
    return s.version ? `${running} (${s.version})` : running
  }
  return t('config.vigem_status_installed')
})

const vigemStateIcon = computed(() => {
  const s = vigem.status
  if (s.installed && s.version_ok) return 'check_circle'
  if (s.installed) return 'warning'
  return 'cancel'
})

onMounted(async () => {
  if (!window.isTauri) return
  vmouse.available = !!window.vmouseDriver
  vigem.available = !!window.vigemDriver
  await Promise.all([
    vmouse.available ? vmouse.refresh() : Promise.resolve(),
    vigem.available ? vigem.refresh() : Promise.resolve(),
  ])
})

onBeforeUnmount(() => {
  vmouse.dispose()
  vigem.dispose()
})

const gamepadOptions = computed(() => {
  const list = [{ value: 'auto', label: t('_common.auto') }]
  if (props.platform === 'linux') list.push({ value: 'ds5', label: t('config.gamepad_ds5') }, { value: 'switch', label: t('config.gamepad_switch') }, { value: 'xone', label: t('config.gamepad_xone') })
  if (props.platform === 'windows') list.push({ value: 'ds5', label: t('config.gamepad_ds5') }, { value: 'ds4', label: t('config.gamepad_ds4') }, { value: 'x360', label: t('config.gamepad_x360') })
  return list
})
// the type cards: the short name above, what is in the brackets as the second line ("X360" / "Xbox 360")
const splitLabel = (label) => { const m = /^(.*?)\s*[（(]([^）)]*)[）)]\s*$/.exec(label); return m ? { label: m[1], text: m[2] } : { label, text: '' } }
const gamepadCards = computed(() => gamepadOptions.value.map((o) => (o.value === 'auto' ? { value: 'auto', label: t('_common.auto'), text: '' } : { value: o.value, label: splitLabel(o.label).label, text: '' })))
// A key that the host leaves out of the configuration means its default: the host writes only what differs from it.
// The sections below read the effective value, or a fresh configuration shows almost nothing.
const on = (value, fallback) => String(value ?? fallback) === 'enabled'
// The DualSense block shows while DualSense is the chosen type, and for "automatic" (the host can resolve it to DualSense)
const dualsenseShown = computed(() => props.platform !== 'macos' && ((config.value.gamepad ?? 'auto') === 'ds5' || (config.value.gamepad ?? 'auto') === 'auto'))
const perGameOptions = computed(() => gamepadOptions.value.filter((o) => o.value !== 'auto').concat([{ value: 'auto', label: t('_common.auto') }]))
</script>

<template>
  <!-- 手柄 -->
  <CfgSub id="gamepad" :title="$t('shell.gamepad')">
    <MSection>
      <MGroup>
        <CfgSwitch :config="config" k="controller" />
      </MGroup>
      <template v-if="on(config.controller, 'enabled')">
        <CfgCards v-if="platform !== 'macos'" :config="config" k="gamepad" :options="gamepadCards" def="auto" quiet :columns="gamepadCards.length > 4 ? 3 : 0">
          <template #art="{ option }"><MControllerArt :type="option.value" /></template>
        </CfgCards>
        <MItem v-if="ds4Hint" icon="info" :title="t(ds4Hint.key)" />
        <DualSenseGamepad v-if="dualsenseShown" :platform="platform" />
        <MGroup>
          <CfgRow v-if="platform !== 'macos'" id="gamepad-pergame" :title="$t('ds5.pergame.title')" icon="apps" />
          <CfgSwitch v-if="(config.gamepad ?? 'auto') === 'ds4'" :config="config" k="ds4_back_as_touchpad_click" def="enabled" />
        </MGroup>
      </template>
    </MSection>
    <!-- what only matters to some pads or hosts: the bottom of the page, not a page of its own -->
    <MSection v-if="on(config.controller, 'enabled')" :title="$t('tabs.advanced')">
      <MGroup>
        <CfgExpert :config="config" k="back_button_timeout" off-text="disabled" />
        <template v-if="(config.gamepad ?? 'auto') === 'auto' && platform === 'windows'">
          <CfgSwitch :config="config" k="motion_as_ds4" />
          <CfgSwitch :config="config" k="touchpad_as_ds4" />
          <CfgExpert :config="config" k="enable_dsu_server" />
          <CfgExpert v-if="on(config.enable_dsu_server, 'disabled')" :config="config" k="dsu_server_port" type="number" placeholder="26760" />
        </template>
      </MGroup>
    </MSection>
    <!-- ViGEmBus virtual gamepad driver (Windows + control panel only) -->
    <MSection v-if="on(config.controller, 'enabled') && vigem.available" title="ViGEmBus">
      <MGroup>
        <MItem :icon="vigemStateIcon" :title="vigemStatusLabel" :text="$t('config.vigem_desc')">
          <template #trail><MIconButton icon="sync" :label="$t('config.vigem_refresh')" :disabled="vigem.loading" @click="vigem.refresh" /></template>
        </MItem>
        <template v-if="!vigem.status.installed || !vigem.status.version_ok">
          <MItem action icon="download" :disabled="vigem.loading || vigem.operating" :title="vigem.operating ? $t('config.vigem_installing') : (vigem.status.installed ? $t('config.vigem_update') : $t('config.vigem_install'))" @click="installVigem(false)" />
        </template>
        <template v-else>
          <MItem action icon="restart_alt" :disabled="vigem.loading || vigem.operating" :title="vigem.operating ? $t('config.vigem_installing') : $t('config.vigem_reinstall')" :text="$t('config.vigem_reinstall_desc')" @click="installVigem(true)" />
          <MItem action icon="delete" :disabled="vigem.loading || vigem.operating" :title="vigem.operating ? $t('config.vigem_uninstalling') : $t('config.vigem_uninstall')" @click="uninstallVigem" />
        </template>
      </MGroup>
    </MSection>
  </CfgSub>

  <!-- 键盘 -->
  <CfgSub id="keyboard" :title="$t('config.input_keyboard_title')">
    <MSection>
      <MGroup>
        <CfgSwitch :config="config" k="keyboard" />
        <template v-if="on(config.keyboard, 'enabled')">
          <CfgExpert v-if="platform === 'windows'" :config="config" k="always_send_scancodes" />
          <CfgExpert :config="config" k="key_rightalt_to_key_win" />
        </template>
      </MGroup>
    </MSection>
    <MSection v-if="on(config.keyboard, 'enabled') && platform === 'windows'" :title="$t('tabs.advanced')">
      <MGroup>
        <CfgExpert :config="config" k="key_repeat_delay" />
        <CfgExpert :config="config" k="key_repeat_frequency" />
      </MGroup>
    </MSection>
  </CfgSub>

  <!-- 鼠标与触控 -->
  <CfgSub id="mouse" :title="$t('config.input_mouse_title')">
    <MSection>
      <MGroup>
        <CfgSwitch :config="config" k="mouse" />
        <CfgSwitch v-if="on(config.mouse, 'enabled')" :config="config" k="high_resolution_scrolling" />
        <CfgSwitch :config="config" k="capture_cursor" :on="true" :off="false" def="true" />
        <CfgExpert v-if="on(config.mouse, 'enabled')" :config="config" k="native_pen_touch" />
      </MGroup>
    </MSection>
    <MSection :title="$t('tabs.advanced')">
      <MGroup>
        <CfgExpert v-if="on(config.mouse, 'enabled') && platform === 'windows'" :config="config" k="native_touchpad_optimization" />
        <CfgExpert :config="config" k="amf_draw_mouse_cursor" :on="true" :off="false" def="false" />
      </MGroup>
    </MSection>
    <!-- Virtual mouse driver -->
    <MSection v-if="on(config.mouse, 'enabled')" :title="$t('config.virtual_mouse')">
      <MGroup>
        <CfgSwitch :config="config" k="virtual_mouse">
          <MChip tone="warn">{{ $t('config.experimental') }}</MChip>
        </CfgSwitch>
        <template v-if="vmouse.available">
          <MItem :icon="vmouse.stateIcon" :title="vmouseStatusLabel">
            <template #trail><MIconButton icon="sync" :label="$t('config.vmouse_refresh')" :disabled="vmouse.loading" @click="vmouse.refresh" /></template>
          </MItem>
          <MItem v-if="!vmouse.status.installed" action icon="download" :disabled="vmouse.loading || vmouse.operating" :title="vmouse.operating ? $t('config.vmouse_installing') : $t('config.vmouse_install')" @click="installVmouse" />
          <MItem v-else action icon="delete" :disabled="vmouse.loading || vmouse.operating" :title="vmouse.operating ? $t('config.vmouse_uninstalling') : $t('config.vmouse_uninstall')" @click="uninstallVmouse" />
        </template>
      </MGroup>
      <MFooter v-if="!vmouse.available">{{ $t('config.vmouse_note') }}</MFooter>
    </MSection>
  </CfgSub>

  <!-- USB 转发: what the host configuration offers (the list of the devices on this computer is the control panel's own page) -->
  <CfgSub v-if="platform === 'windows'" id="usb" :title="$t('shell.usb_forwarding')">
    <MSection>
      <MGroup>
        <CfgSwitch :config="config" k="usb_forwarding_enabled" help="config.usb_forwarding_description" />
        <CfgExpert v-if="on(config.usb_forwarding_enabled, 'disabled')" :config="config" k="usb_forwarding_port" type="number" placeholder="0" help="config.usb_forwarding_port_hint" />
      </MGroup>
    </MSection>
  </CfgSub>

  <DualSenseSubs v-if="dualsenseShown" />
  <GamepadPerGame :options="perGameOptions" />
</template>
