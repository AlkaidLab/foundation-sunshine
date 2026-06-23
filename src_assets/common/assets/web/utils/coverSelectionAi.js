import { searchAllCovers } from './coverSearch.js'
import { API_ENDPOINTS } from './constants.js'
import { getCoverSearchCandidates } from './gameMetadataAi.js'
import { buildLocalizedInstruction, getCurrentLocale, getPromptLanguageName } from './aiLocale.js'
import { createAiCache } from './aiCache.js'

const MAX_SEARCH_TERMS = 3
const MAX_COVERS_PER_TERM = 6
const MAX_AI_CANDIDATES = 12
const coverCache = createAiCache('cover-selection', { version: 'v1' })

function normalizeTitle(value) {
  return String(value || '')
    .toLowerCase()
    .replace(/[^a-z0-9\u4e00-\u9fff]+/g, ' ')
    .replace(/\s+/g, ' ')
    .trim()
}

function scoreCandidate(app, candidate) {
  const title = normalizeTitle(candidate.name)
  const names = [
    app?.['canonical-name'],
    app?.name,
    app?.['original-name'],
    ...(Array.isArray(app?.['cover-search-terms']) ? app['cover-search-terms'] : []),
  ].map(normalizeTitle).filter(Boolean)

  let score = candidate.source === 'igdb' ? 3 : 2
  for (const name of names) {
    if (title === name) score += 20
    else if (title.startsWith(name) || name.startsWith(title)) score += 12
    else if (title.includes(name) || name.includes(title)) score += 8
  }
  return score
}

function dedupeCandidates(candidates) {
  const seen = new Set()
  const result = []

  for (const candidate of candidates) {
    const key = candidate.saveUrl || candidate.url || candidate.key
    if (!key || seen.has(key)) continue
    seen.add(key)
    result.push(candidate)
  }

  return result
}

export function pickFallbackCoverCandidate(app, candidates) {
  if (!Array.isArray(candidates) || candidates.length === 0) return null
  return [...candidates].sort((a, b) => scoreCandidate(app, b) - scoreCandidate(app, a))[0]
}

function compactCandidate(candidate, index) {
  return {
    id: String(index),
    name: candidate.name,
    source: candidate.source,
    searchTerm: candidate.searchTerm,
  }
}

function buildCoverCacheKey(app, locale) {
  return coverCache.makeKey({
    locale,
    name: app?.name || '',
    originalName: app?.['original-name'] || '',
    canonicalName: app?.['canonical-name'] || '',
    searchTerms: getCoverSearchCandidates(app).slice(0, MAX_SEARCH_TERMS),
    platform: app?.['app-type'] || '',
  })
}

function buildCoverSelectionPrompt(locale = getCurrentLocale()) {
  const languageName = getPromptLanguageName(locale)

  return [
    'You select the best cover art match for a Sunshine game streaming library.',
    'Return only one valid JSON object, with no markdown.',
    buildLocalizedInstruction(locale),
    `Short reasons should be in ${languageName}.`,
    'Choose the candidate that best matches the target game, not DLC, soundtrack, tool, demo, or unrelated same-name software.',
    'Prefer official game entries and exact title matches.',
    'If candidates are equally plausible, prefer box-art/library style covers over wide headers.',
  ].join(' ')
}

async function collectCoverCandidates(app) {
  const searchTerms = getCoverSearchCandidates(app).slice(0, MAX_SEARCH_TERMS)
  const candidates = []

  for (const term of searchTerms) {
    const results = await searchAllCovers(term)
    const covers = [...(results.igdb || []), ...(results.steam || [])].slice(0, MAX_COVERS_PER_TERM)
    for (const cover of covers) {
      candidates.push({
        ...cover,
        searchTerm: term,
      })
    }
  }

  return dedupeCandidates(candidates)
    .map((candidate, index) => ({ ...candidate, fallbackScore: scoreCandidate(app, candidate), originalIndex: index }))
    .sort((a, b) => b.fallbackScore - a.fallbackScore)
    .slice(0, MAX_AI_CANDIDATES)
}

async function askAiToPickCover(app, candidates) {
  const locale = getCurrentLocale()
  const response = await fetch(API_ENDPOINTS.AI_CHAT_COMPLETIONS, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      messages: [
        { role: 'system', content: buildCoverSelectionPrompt(locale) },
        {
          role: 'user',
          content: JSON.stringify({
            task: 'select_best_cover',
            locale,
            target: {
              name: app?.name || '',
              originalName: app?.['original-name'] || '',
              canonicalName: app?.['canonical-name'] || '',
              searchTerms: getCoverSearchCandidates(app),
              platform: app?.['app-type'] || '',
            },
            candidates: candidates.map(compactCandidate),
            outputSchema: {
              selectedId: 'id string from candidates',
              confidence: 0.0,
              reason: 'short user-facing reason',
            },
          }),
        },
      ],
      temperature: 0.1,
      max_tokens: 1024,
    }),
  })

  const data = await response.json().catch(() => ({}))
  if (!response.ok) {
    const message = typeof data.error === 'string' ? data.error : data.error?.message
    throw new Error(message || `AI cover selection failed: ${response.status}`)
  }

  const content = data.choices?.[0]?.message?.content || ''
  const start = content.indexOf('{')
  const end = content.lastIndexOf('}')
  if (start === -1 || end === -1 || end <= start) {
    throw new Error('AI cover selection response did not contain JSON')
  }

  const parsed = JSON.parse(content.slice(start, end + 1))
  const selected = candidates[Number(parsed.selectedId)]
  return selected ? { ...selected, aiCoverConfidence: Number(parsed.confidence) || 0, aiCoverReason: parsed.reason || '' } : null
}

export async function findBestCoverForApp(app) {
  const locale = getCurrentLocale()
  const cacheKey = buildCoverCacheKey(app, locale)
  const cached = coverCache.get(cacheKey)
  if (cached !== undefined) {
    return cached
  }

  let candidates = []
  try {
    candidates = await collectCoverCandidates(app)
  } catch (error) {
    console.warn('Cover candidate search failed; skipping cover selection:', error)
    return null
  }
  if (candidates.length === 0) {
    coverCache.set(cacheKey, null)
    return null
  }
  if (candidates.length === 1) {
    coverCache.set(cacheKey, candidates[0])
    return candidates[0]
  }

  try {
    const selected = (await askAiToPickCover(app, candidates)) || pickFallbackCoverCandidate(app, candidates)
    coverCache.set(cacheKey, selected)
    return selected
  } catch (error) {
    console.warn('AI cover selection failed; using fallback cover candidate:', error)
    const selected = pickFallbackCoverCandidate(app, candidates)
    coverCache.set(cacheKey, selected)
    return selected
  }
}
