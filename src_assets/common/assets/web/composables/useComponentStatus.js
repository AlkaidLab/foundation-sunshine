import { createComponentStatus, microphoneState, registerComponentStatus } from 'foundation-ui/vue'
import { apiFetch, apiJson, parseJsonResponse } from '../utils/apiFetch.js'
import { askConfirm } from './askDialog.js'
import { installFromFile, uninstallChoice } from '../utils/sparxie/componentInstall.js'
import { useShellLive } from './useShellLive.js'
import { useUpdateStatus } from './useUpdateStatus.js'

// The component status of the Web UI: the external components, as far as the host reports them through its own routes (the virtual display, the Vulkan HDR component, the microphone). The
// drivers and the NVIDIA runtimes are managed by the control panel and have no route on the host: they have no source here and say so on their cards. The DualSense, encoder and host checks
// are not components: they feed the gamepad page, the encoders page and the about page.
let controller = null

const dualsenseRaw = (snapshot) => {
  if (!snapshot || typeof snapshot !== 'object') return null
  const active = Number(snapshot.active_count) || 0
  return {
    state: snapshot.native_path_compiled === false ? 'native_absent' : 'ready',
    host: { reachable: true, tuning_available: snapshot.status !== false, native_compiled: snapshot.native_path_compiled, native_active: active > 0, active_controllers: active },
  }
}

export function useComponentStatus({ locale, openPage, openAbout } = {}) {
  if (controller) return controller
  // the page that owns an update sheet sets `hooks.openUpdate`; without one the host row leads to the about page, which has it
  const hooks = { openUpdate: null }
  const live = useShellLive()
  const update = useUpdateStatus()
  const config = async (fresh) => (fresh || !live.config.value ? (live.config.value = await apiJson('/api/config')) : live.config.value)
  const encoderOf = (cfg) => (cfg ? { active: cfg.active_encoder, hevc: cfg.hevc_mode, av1: cfg.av1_mode } : null)
  const hostOf = (cfg) => ({ version: cfg?.version, reachable: !!cfg, update: update.state.value === 'available' ? 'available' : update.state.value === 'latest' ? 'latest' : update.state.value === 'error' ? 'error' : '', next: update.nextVersion.value || '' })

  const sparxieCall = async (url, body) => {
    const response = await apiFetch(url, body === undefined ? {} : { method: 'POST', body })
    const payload = await parseJsonResponse(response, {})
    if (!response.ok) return { ...payload, ok: false }
    return payload
  }
  const sparxieTexts = () => controller.text().sparxie
  controller = registerComponentStatus(createComponentStatus({
    locale,
    openPage,
    sources: {
      vdd: () => apiJson('/api/vdd/status'),
      vulkan: () => apiJson('/api/vulkan-hdr-bridge'),
      dualsense: async () => dualsenseRaw(await apiJson('/api/dualsense/tuning')),
      mic: async () => { const s = await apiJson('/api/microphone/status'); return { state: microphoneState(s), backend: s?.active_backend || s?.configured_backend } },
      encoder: { read: async () => encoderOf(await config(false)), fresh: async () => encoderOf(await config(true)) },
      host: { read: async () => hostOf(await config(false)), fresh: async () => { const cfg = await config(true); await update.check(false); return hostOf(cfg) } },
      sparxie: () => sparxieCall('/api/sparxie/status'),
    },
    actions: {
      vdd: { note: 'viaPanel' }, usb: { note: 'viaPanel' }, vigem: { note: 'viaPanel' }, mouse: { note: 'viaPanel' }, vulkan: { note: 'viaPanel' }, mic: { note: 'viaPanel' },
      host: { update: { run: () => (hooks.openUpdate ? hooks.openUpdate() : openAbout?.()) } },
      sparxie: {
        import: {
          label: (S, row) => (!row || row.state === 'missing' || row.state === 'unknown' ? S.sparxie.fromFile : S.sparxie.update),
          run: async () => {
            const { useNotify } = await import('foundation-ui/notify')
            const notify = useNotify()
            const texts = sparxieTexts()
            const result = await installFromFile({
              status: await sparxieCall('/api/sparxie/status'),
              pick: async () => {
                const picked = await sparxieCall('/api/sparxie/pick', { kind: 'file' })
                return picked && picked.path ? picked.path : ''
              },
              ask: (message, title, confirmText) => askConfirm(message, { title, confirmText }),
              post: (body) => sparxieCall('/api/sparxie/install', { ...body, wait: true }),
              texts,
            })
            if (result && result.ok) notify.snack(texts.done)
            else if (result && result.error_code && result.error_code !== 'cancelled') notify.snack(texts.failed + ' ' + (result.error || result.error_code))
            await controller.check('sparxie')
          },
        },
        remove: {
          label: (S) => S.uninstall.yes,
          run: async () => {
            const { useNotify } = await import('foundation-ui/notify')
            const notify = useNotify()
            const texts = sparxieTexts()
            const result = await uninstallChoice(
              (body) => sparxieCall('/api/sparxie/uninstall', body),
              async (message, title, confirmText, cancelText) => {
                const deleted = await askConfirm(message, { title, confirmText, cancelText, danger: true })
                return deleted ? 'delete' : 'keep'
              },
              texts,
            )
            if (result && result.ok) notify.snack(texts.removed)
            else if (result && result.error_code && result.error_code !== 'cancelled') notify.snack(texts.failed)
            await controller.check('sparxie')
          },
        },
      },
    },
  }))
  controller.hooks = hooks
  return controller
}
