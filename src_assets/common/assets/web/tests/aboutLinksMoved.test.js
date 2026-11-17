import './helpers/foundationUi.mjs'
import assert from 'node:assert/strict'
import test from 'node:test'
import { CLIENT_PLATFORMS } from '../config/clientDownloads.js'
import { PARTNER_CARDS, PARTNER_CLIENTS } from '../config/communityLinks.js'

// The About page lists only this software (and the licences it owes). The partner clients and the team's projects that it used to list now live on the
// client downloads page; this test keeps it so: every link the About page dropped must be reachable from a client of that page.
const { TEAM_PROJECTS, THANKS } = await import('foundation-ui/about/index.js')

const reachable = new Set()
const collect = (client) => {
  for (const key of ['project', 'release']) if (client[key]) reachable.add(client[key])
  for (const link of client.links || []) reachable.add(link.url)
  for (const alt of client.alternatives || []) collect(alt)
  const urls = client.getDownloadUrls?.('en') || {}
  for (const url of Object.values(urls)) if (url) reachable.add(url)
  const zh = client.getDownloadUrls?.('zh') || {}
  for (const url of Object.values(zh)) if (url) reachable.add(url)
}
CLIENT_PLATFORMS.forEach(collect)
PARTNER_CARDS.forEach(collect) // the Crown and OpenHarmony editions are cards of their own
const norm = (url) => String(url).replace(/#readme$/, '').replace(/\/$/, '')
const has = (url) => [...reachable].some((r) => norm(r) === norm(url) || norm(r).startsWith(norm(url) + '/') || norm(url).startsWith(norm(r) + '/'))

test('every link of a partner client is on the downloads page (the About page lists none of them)', () => {
  for (const client of PARTNER_CLIENTS) for (const link of client.links) assert.ok(has(link.url), `${client.name}: ${link.url}`)
})
test('every project of the team the About page dropped is on the downloads page, or stays in its thanks list', () => {
  const thanked = THANKS.map((t) => t.url)
  for (const projects of Object.values(TEAM_PROJECTS)) for (const project of projects) assert.ok(has(project.url) || thanked.includes(project.url), project.url)
})

// the list the page shows is the one of the package: the team's clients first (the About page thanks the partners, the page lists them with the badge)
const { clientsForOs } = await import('foundation-ui/clients')
test('filtered by a system the page lists the team\'s clients first and every partner client of that system after them', () => {
  const partnerIds = new Set(PARTNER_CLIENTS.map((c) => c.name))
  assert.ok(partnerIds.size > 0)
  for (const os of ['windows', 'macos', 'linux', 'android', 'ios', 'harmony', 'tv']) {
    const list = clientsForOs(os)
    const firstPartner = list.findIndex((x) => x.client.group === 'partner')
    if (firstPartner >= 0) assert.ok(list.slice(firstPartner).every((x) => x.client.group === 'partner'), os)
  }
})
