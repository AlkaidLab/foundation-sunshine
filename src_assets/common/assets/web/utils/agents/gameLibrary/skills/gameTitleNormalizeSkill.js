import { enhanceScannedGameNames } from '../../../gameMetadataAi.js'

export const GAME_TITLE_NORMALIZE_SKILL_ID = 'game.title.normalize'

function countNameChanges(before, after) {
  return after.reduce((count, app, index) => {
    const previous = before[index] || {}
    return app?.name !== previous.name || app?.['canonical-name'] !== previous['canonical-name'] ? count + 1 : count
  }, 0)
}

export function createGameTitleNormalizeSkill(options = {}) {
  const enhanceNames = options.enhanceNames || enhanceScannedGameNames

  return {
    id: GAME_TITLE_NORMALIZE_SKILL_ID,
    type: 'metadata',
    label: 'Game title normalization',

    async run(context) {
      const before = context.apps || []
      const apps = await enhanceNames(before)
      const changed = countNameChanges(before, apps)

      context.events?.push({
        skillId: GAME_TITLE_NORMALIZE_SKILL_ID,
        type: 'titles:enhanced',
        changed,
      })
      context.options?.onTitlesEnhanced?.(apps, { changed })

      return {
        ...context,
        apps,
        stats: {
          ...(context.stats || {}),
          titleChanges: changed,
        },
      }
    },
  }
}
