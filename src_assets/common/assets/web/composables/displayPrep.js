// How the displays are handled while a stream runs (display_device_prep): the choices the 显示器配置 row offers.
// 扩展显示器 (ensure_secondary) extends the desktop onto the base virtual display; the host supports it for that display only (display_device/parsed_config.h), so it is offered on Windows
// when the base virtual display is the one that is streamed. A configuration that already holds it keeps the choice on every target, so a saved value is never hidden.
export const PREP_BASIC = Object.freeze(['no_operation', 'ensure_active', 'ensure_primary', 'ensure_only_display'])
export const PREP_SECONDARY = 'ensure_secondary'

export function prepChoices({ windows = true, virtual = false, current = '' } = {}) {
  return windows && (virtual || current === PREP_SECONDARY) ? [...PREP_BASIC, PREP_SECONDARY] : [...PREP_BASIC]
}
