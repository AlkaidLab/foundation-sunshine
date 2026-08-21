export const PER_APP_GAMEPAD_MODES = Object.freeze([
  { value: '', labelKey: 'apps.gamepad_mode_inherit' },
  { value: 'auto', labelKey: 'apps.gamepad_mode_auto' },
  { value: 'x360', labelKey: 'apps.gamepad_mode_x360' },
  { value: 'ds4', labelKey: 'apps.gamepad_mode_ds4' },
  { value: 'ds5', labelKey: 'config.gamepad_ds5' },
])

const VALID_PER_APP_GAMEPAD_MODES = new Set(
  PER_APP_GAMEPAD_MODES.map(({ value }) => value),
)

export const normalizePerAppGamepadMode = (value) =>
  VALID_PER_APP_GAMEPAD_MODES.has(value) ? value : ''
