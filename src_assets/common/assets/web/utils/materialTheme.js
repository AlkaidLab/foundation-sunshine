import {
  QuantizerCelebi,
  Score,
  argbFromRgb,
  hexFromArgb,
  themeFromSourceColor,
} from '@material/material-color-utilities'

// Precomputed with the same extractor for the default wallpaper. The wallpaper CDN
// does not allow canvas reads, so the default can still have its matching theme.
export const DEFAULT_WALLPAPER_SEED = 0xff4a4b79

// Use the same Celebi quantizer and ranked seed selection as the desktop UI.
export function materialSeedFromImageData({ data }) {
  const pixels = []
  for (let offset = 0; offset < data.length; offset += 4) {
    if (data[offset + 3] < 128) continue
    pixels.push(argbFromRgb(data[offset], data[offset + 1], data[offset + 2]))
  }
  if (!pixels.length) return null
  return Score.score(QuantizerCelebi.quantize(pixels, 128), { desired: 6 })[0] ?? null
}

// Content-scoped wallpaper tones. Only home page content reads these (see
// home.less); brand --ui-* variables stay untouched everywhere else.
export function materialThemeTokens(seed, dark = false) {
  const theme = themeFromSourceColor(seed)
  const scheme = dark ? theme.schemes.dark : theme.schemes.light
  const background = theme.palettes.neutral.tone(dark ? 6 : 98)

  return {
    '--mt-page-bg': hexFromArgb(background),
    '--mt-text-primary': hexFromArgb(scheme.onSurface),
    '--mt-text-secondary': hexFromArgb(scheme.onSurfaceVariant),
  }
}
