import { API_ENDPOINTS } from './constants.js'
import { buildLocalizedInstruction, getCurrentLocale, getPromptLanguageName } from './aiLocale.js'
import { createAiCache } from './aiCache.js'

const MAX_ITEMS_PER_BATCH = 20
const MIN_RENAME_CONFIDENCE = 0.82
const metadataCache = createAiCache('game-metadata', { version: 'v1' })

function compactAppForPrompt(app, index) {
  return {
    id: String(index),
    name: app.name || '',
    cmd: app.cmd || '',
    workingDir: app['working-dir'] || app.working_dir || '',
    sourcePath: app.source_path || '',
    platform: app['app-type'] || app.platform || '',
    isGame: app['is-game'] === true,
  }
}

function buildMetadataCacheKey(app, locale) {
  const compact = compactAppForPrompt(app, 'cache')
  delete compact.id
  return metadataCache.makeKey({ locale, app: compact })
}

function splitIntoBatches(items, size = MAX_ITEMS_PER_BATCH) {
  const batches = []
  for (let i = 0; i < items.length; i += size) {
    batches.push(items.slice(i, i + size))
  }
  return batches
}

function extractJsonObject(text) {
  if (!text) return null
  const trimmed = text.trim()
  if (trimmed.startsWith('{') && trimmed.endsWith('}')) {
    return trimmed
  }

  const start = trimmed.indexOf('{')
  const end = trimmed.lastIndexOf('}')
  if (start === -1 || end === -1 || end <= start) {
    return null
  }
  return trimmed.slice(start, end + 1)
}

function normalizeSearchTerms(terms, fallbackName) {
  const result = []
  if (Array.isArray(terms)) {
    for (const term of terms) {
      if (typeof term === 'string' && term.trim()) {
        result.push(term.trim())
      }
    }
  }
  if (fallbackName && !result.includes(fallbackName)) {
    result.push(fallbackName)
  }
  return Array.from(new Set(result)).slice(0, 5)
}

function normalizeAiItem(item, original) {
  if (!item || typeof item !== 'object') return null

  const confidence = Math.max(0, Math.min(1, Number(item.confidence) || 0))
  const canonicalName = String(item.canonicalName || item.canonical_name || '').trim()
  const displayName = String(item.displayName || item.display_name || canonicalName || original.name || '').trim()
  const originalName = original.name || ''

  if (!displayName && !canonicalName) return null

  const searchTerms = normalizeSearchTerms(item.searchTerms || item.search_terms, canonicalName || displayName || originalName)
  const shouldRename = confidence >= MIN_RENAME_CONFIDENCE && displayName && displayName !== originalName

  return {
    ...original,
    ...(shouldRename && { name: displayName }),
    'original-name': original['original-name'] || originalName,
    'canonical-name': canonicalName || displayName,
    'cover-search-terms': searchTerms,
    'ai-confidence': confidence,
    'ai-is-game': item.isGame ?? item.is_game,
    'ai-reason': String(item.reason || '').trim(),
  }
}

function buildMetadataSystemPrompt(locale = getCurrentLocale()) {
  const languageName = getPromptLanguageName(locale)

  return [
    'You clean game/application titles for a Sunshine game streaming library.',
    'Return only one valid JSON object, with no markdown.',
    buildLocalizedInstruction(locale),
    `Prefer official ${languageName} localized titles for displayName when they are well-known and unambiguous.`,
    'If an official localized title is not obvious, keep the official international/English title as displayName.',
    'canonicalName should be the stable official international title when known.',
    `searchTerms should include useful ${languageName} and English cover-search aliases when available.`,
    'Remove launcher noise, exe suffixes, edition noise only when obvious, and path artifacts.',
    'Never invent a title when uncertain; keep the original name and lower confidence.',
  ].join(' ')
}

async function callAiForBatch(batch) {
  const promptItems = batch.map(({ app, index }) => compactAppForPrompt(app, index))
  const locale = getCurrentLocale()
  const languageName = getPromptLanguageName(locale)
  const body = {
    messages: [
      {
        role: 'system',
        content: buildMetadataSystemPrompt(locale),
      },
      {
        role: 'user',
        content: JSON.stringify({
          task: 'normalize_game_titles',
          locale,
          userFacingLanguage: languageName,
          outputSchema: {
            items: [
              {
                id: 'same string id from input',
                displayName: 'clean title to show in UI',
                canonicalName: 'canonical official game title',
                searchTerms: ['best cover search term', 'alternate title'],
                isGame: true,
                confidence: 0.0,
                reason: 'short reason',
              },
            ],
          },
          items: promptItems,
        }),
      },
    ],
    temperature: 0.1,
    max_tokens: 4096,
  }

  const response = await fetch(API_ENDPOINTS.AI_CHAT_COMPLETIONS, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  })

  const data = await response.json().catch(() => ({}))
  if (!response.ok) {
    const message = typeof data.error === 'string' ? data.error : data.error?.message
    throw new Error(message || `AI metadata request failed: ${response.status}`)
  }

  const content = data.choices?.[0]?.message?.content || ''
  const jsonText = extractJsonObject(content)
  if (!jsonText) {
    throw new Error('AI metadata response did not contain JSON')
  }

  const parsed = JSON.parse(jsonText)
  return Array.isArray(parsed.items) ? parsed.items : []
}

export async function enhanceScannedGameNames(apps) {
  if (!Array.isArray(apps) || apps.length === 0) {
    return apps
  }

  const locale = getCurrentLocale()
  const misses = []
  const enhanced = [...apps]

  apps.forEach((app, index) => {
    if (app?.['user-override'] === true) {
      enhanced[index] = app
      return
    }

    const cacheKey = buildMetadataCacheKey(app, locale)
    const cached = metadataCache.get(cacheKey)
    const normalized = cached ? normalizeAiItem(cached, app) : null
    if (normalized) {
      enhanced[index] = normalized
      return
    }
    misses.push({ app, index, cacheKey })
  })

  const batches = splitIntoBatches(misses)
  for (const batch of batches) {
    let aiItems = []
    try {
      aiItems = await callAiForBatch(batch)
    } catch (error) {
      console.warn('AI metadata enhancement failed for batch; skipping:', error)
      continue
    }
    const byId = new Map(aiItems.map((item) => [String(item.id), item]))

    for (const { app, index, cacheKey } of batch) {
      const aiItem = byId.get(String(index))
      const normalized = normalizeAiItem(aiItem, app)
      if (normalized) {
        enhanced[index] = normalized
        metadataCache.set(cacheKey, aiItem)
      }
    }
  }

  return enhanced
}

export function getCoverSearchCandidates(app) {
  const terms = Array.isArray(app?.['cover-search-terms']) ? app['cover-search-terms'] : []
  return Array.from(new Set([
    ...terms,
    app?.['canonical-name'],
    app?.name,
    app?.['original-name'],
  ].filter((term) => typeof term === 'string' && term.trim()).map((term) => term.trim())))
}
