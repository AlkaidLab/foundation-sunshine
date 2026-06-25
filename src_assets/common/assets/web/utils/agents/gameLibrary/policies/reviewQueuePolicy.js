export const GAME_RESOURCE_REVIEW_THRESHOLDS = {
  nameConfidence: 0.82,
  coverConfidence: 0.7,
}

const ZH_REVIEW_MESSAGES = {
  lowNameConfidence: (percent) => `\u540d\u79f0\u7f6e\u4fe1\u5ea6 ${percent}%`,
  missingCanonicalName: '\u7f3a\u5c11\u89c4\u8303\u540d\u79f0',
  missingCover: '\u7f3a\u5c11\u5c01\u9762',
  lowCoverConfidence: (percent) => `\u5c01\u9762\u7f6e\u4fe1\u5ea6 ${percent}%`,
}

const EN_REVIEW_MESSAGES = {
  lowNameConfidence: (percent) => `Low name confidence ${percent}%`,
  missingCanonicalName: 'Missing canonical name',
  missingCover: 'Missing cover',
  lowCoverConfidence: (percent) => `Low cover confidence ${percent}%`,
}

function hasNumericValue(value) {
  if (value === null || value === undefined) return false
  if (typeof value === 'string' && value.trim() === '') return false
  return Number.isFinite(Number(value))
}

function getReviewMessages(locale = 'en') {
  return String(locale || '').toLowerCase().startsWith('zh') ? ZH_REVIEW_MESSAGES : EN_REVIEW_MESSAGES
}

export function getGameResourceReviewReasons(app, options = {}) {
  const thresholds = {
    ...GAME_RESOURCE_REVIEW_THRESHOLDS,
    ...(options.thresholds || {}),
  }
  const messages = getReviewMessages(options.locale)
  const reasons = []
  const nameConfidence = Number(app?.['ai-confidence'])
  const coverConfidence = hasNumericValue(app?.['ai-cover-confidence'])
    ? Number(app?.['ai-cover-confidence'])
    : Number(app?.['cover-match-confidence'])

  if (hasNumericValue(app?.['ai-confidence']) && nameConfidence < thresholds.nameConfidence) {
    reasons.push(messages.lowNameConfidence(Math.round(nameConfidence * 100)))
  }

  if (app?.['is-game'] === true && !app?.['canonical-name']) {
    reasons.push(messages.missingCanonicalName)
  }

  if (app?.['is-game'] === true && !app?.['image-path']) {
    reasons.push(messages.missingCover)
  }

  const hasCoverConfidence = hasNumericValue(app?.['ai-cover-confidence']) || hasNumericValue(app?.['cover-match-confidence'])
  if (hasCoverConfidence && coverConfidence < thresholds.coverConfidence) {
    reasons.push(messages.lowCoverConfidence(Math.round(coverConfidence * 100)))
  }

  return reasons
}

export function needsGameResourceReview(app, options = {}) {
  return getGameResourceReviewReasons(app, options).length > 0
}
