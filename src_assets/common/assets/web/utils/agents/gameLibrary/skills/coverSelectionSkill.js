import { findBestCoverForApp } from '../../../coverSelectionAi.js'

export const COVER_SELECTION_SKILL_ID = 'game.cover.select'

export function getGameResourceKey(app, index) {
  if (app?.['__scan-key']) return app['__scan-key']
  const stablePart = app?.source_path || app?.cmd || app?.name || 'app'
  return `${stablePart}-${index}`
}

export function applyCoverToGameResource(app, cover) {
  const imagePath = cover?.saveUrl || cover?.url || ''
  if (!imagePath) return app

  return {
    ...app,
    'image-path': imagePath,
    'cover-source': cover.source || '',
    'cover-match-name': cover.name || '',
    'cover-search-term': cover.searchTerm || '',
    'ai-cover-confidence': cover.aiCoverConfidence || 0,
    'ai-cover-reason': cover.aiCoverReason || '',
    'cover-match-confidence': cover.coverMatchConfidence ?? cover.matchConfidence ?? 0,
    'cover-match-relation': cover.coverMatchRelation ?? cover.matchRelation ?? '',
    'cover-match-reason': cover.coverMatchReason ?? cover.matchReason ?? '',
  }
}

export function createCoverSelectionSkill(options = {}) {
  const findCover = options.findCover || findBestCoverForApp

  return {
    id: COVER_SELECTION_SKILL_ID,
    type: 'asset',
    label: 'Game cover selection',

    findCover(app) {
      return findCover(app)
    },

    async run(context) {
      const apps = [...(context.apps || [])]
      let coversFound = 0

      const results = await Promise.allSettled(apps.map(async (app, index) => {
        const key = getGameResourceKey(app, index)

        if (app?.['user-override'] === true && app?.['image-path']) {
          coversFound += 1
          return { key, skipped: true, app }
        }

        const cover = await findCover(app)
        const next = applyCoverToGameResource(app, cover)
        if (next !== app) {
          apps[index] = next
          coversFound += 1
          context.options?.onCoverResolved?.(next, {
            app,
            cover,
            index,
            key,
          })
        }
        return { key, cover, app: next }
      }))

      const failures = results.filter((result) => result.status === 'rejected')
      for (const failure of failures) {
        context.options?.onSkillError?.(COVER_SELECTION_SKILL_ID, failure.reason)
      }

      context.events?.push({
        skillId: COVER_SELECTION_SKILL_ID,
        type: 'covers:selected',
        coversFound,
        failures: failures.length,
      })

      return {
        ...context,
        apps,
        stats: {
          ...(context.stats || {}),
          coversFound,
          coverFailures: failures.length,
        },
      }
    },
  }
}
