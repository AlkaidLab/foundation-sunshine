import { applyScanOverrides, learnScanOverride } from '../../../scanOverrides.js'

export const SCAN_OVERRIDE_MEMORY_SKILL_ID = 'game.scan.memory'

export function createScanOverrideMemorySkill(options = {}) {
  const applyOverrides = options.applyOverrides || applyScanOverrides
  const learnOverride = options.learnOverride || learnScanOverride

  return {
    id: SCAN_OVERRIDE_MEMORY_SKILL_ID,
    type: 'memory',
    label: 'Confirmed scan overrides',

    apply(apps) {
      return applyOverrides(apps)
    },

    remember(scannedApp, finalApp) {
      return learnOverride(scannedApp, finalApp)
    },

    async run(context) {
      const apps = applyOverrides(context.apps || [])

      context.events?.push({
        skillId: SCAN_OVERRIDE_MEMORY_SKILL_ID,
        type: 'memory:applied',
      })

      return {
        ...context,
        apps,
      }
    },
  }
}
