const WHATS_CHANGED_HEADING =
  /^\s*<h[1-6][^>]*>\s*What(?:'|’|&(?:#39|apos);)?s Changed\s*<\/h[1-6]>\s*/i
const FULL_CHANGELOG_PARAGRAPH =
  /<p>\s*<strong>\s*Full Changelog\s*:?\s*<\/strong>\s*:?\s*[\s\S]*?<\/p>/i
const CONTRIBUTORS_HEADING =
  /<h([1-6])[^>]*>\s*(?:New\s+)?Contributors\s*<\/h\1>/i
const TRAILING_RULES = /(?:\s*<hr\s*\/?>\s*)+$/i

export const extractReleaseDetails = (body = '') => {
  let notes = body.replace(WHATS_CHANGED_HEADING, '')
  let fullChangelog = ''

  const fullChangelogParagraph = notes.match(FULL_CHANGELOG_PARAGRAPH)
  if (fullChangelogParagraph) {
    fullChangelog = fullChangelogParagraph[0].match(/<a\b[^>]*href="([^"]+)"/i)?.[1] || ''
    notes = notes.replace(fullChangelogParagraph[0], '').trim()
  }

  let contributors = ''
  const contributorsHeading = notes.match(CONTRIBUTORS_HEADING)
  if (contributorsHeading) {
    contributors = notes
      .slice(contributorsHeading.index + contributorsHeading[0].length)
      .replace(TRAILING_RULES, '')
      .trim()
    notes = notes.slice(0, contributorsHeading.index).trim()
  }

  notes = notes.replace(TRAILING_RULES, '').trim()

  return { notes, fullChangelog, contributors }
}
