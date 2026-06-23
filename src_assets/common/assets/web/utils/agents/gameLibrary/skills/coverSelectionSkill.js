import { findBestCoverForApp } from '../../../coverSelectionAi.js'

export const COVER_SELECTION_SKILL_ID = 'game.cover.select'
const DEFAULT_COVER_CONCURRENCY = 4

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
  const defaultConcurrency = Math.max(1, Number(options.concurrency) || DEFAULT_COVER_CONCURRENCY)

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
      let cursor = 0
      const results = []
      const concurrency = Math.min(
        Math.max(1, Number(context.options?.coverConcurrency) || defaultConcurrency),
        apps.length || 1
      )

      const processApp = async (app, index) => {
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
      }

      const runWorker = async () => {
        while (cursor < apps.length) {
          const index = cursor
          cursor += 1
          try {
            results[index] = {
              status: 'fulfilled',
              value: await processApp(apps[index], index),
            }
          } catch (reason) {
            results[index] = { status: 'rejected', reason }
          }
        }
      }

      await Promise.all(Array.from({ length: concurrency }, runWorker))

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
