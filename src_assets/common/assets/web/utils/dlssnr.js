// Keep per-app values consistent between the editor and saved apps.json.
export function normalizeDlssnrConfig(value) {
  const config = value && typeof value === 'object' ? value : {};
  const number = (key, fallback, max = 1, integer = false) => {
    const raw = config[key];
    const parsed = raw === '' || raw == null ? fallback : Number(raw);
    const result = Math.max(0, Math.min(max, Number.isFinite(parsed) ? parsed : fallback));
    return integer ? Math.floor(result) : result;
  };
  return {
    mode: ['inherit', 'on', 'off'].includes(config.mode) ? config.mode : 'inherit',
    style: number('style', 0, 4, true),
    'motion-quality': number('motion-quality', 0, 3, true),
    intensity: number('intensity', 1),
    'local-tone-strength': number('local-tone-strength', 1),
    'local-structure-strength': number('local-structure-strength', 1),
    'skin-structure-strength': number('skin-structure-strength', 0),
    'auto-mask': config['auto-mask'] === true || config['auto-mask'] === 'true',
    'ui-correction': config['ui-correction'] === true || config['ui-correction'] === 'true',
  };
}
