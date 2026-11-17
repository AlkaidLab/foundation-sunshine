import { computed, ref } from 'vue'
import { assetOsOf, osName, releaseRows } from 'foundation-ui/clients'

// The popup of a client card on the downloads page lists that client's files. The card is { client, target } (target is null in the list without a filter).
// The page reads the release data once per product; this module turns it into what the popup shows, so the popup fills in place when the data arrives late.

/** "macOS", "Android", "TV · Steam Deck": the name of the system a target runs on. */
export const targetName = (target, locale = 'en') => osName(target.os, locale) + (target.platform === 'steam' ? ' · Steam Deck' : target.platform === 'quest' ? ' · Quest' : '')

/**
 * The groups of files the popup lists, one per system of the card (the card's target, or every target of the client), the same system once:
 * [{ os, name, rows, state }]. `state` is 'ready' (rows to show), 'loading' (the release data is on its way: the popup shows a skeleton) or 'failed' (read, but no files for this
 * system: the popup shows the links). A client without release data (a store, a partner's page) has no groups.
 *   assets   the product's release assets ([] until read)
 *   loaded   whether the product's release data has been read (with or without a result)
 */
export function fileGroups(card, { assets, loaded, locale = 'en' }) {
  const { client, target } = card
  if (!client.product) return []
  const seen = new Set()
  const groups = []
  for (const t of target ? [target] : client.targets) {
    const os = assetOsOf(t)
    if (seen.has(os)) continue
    seen.add(os)
    const rows = releaseRows(assets, os, locale)
    groups.push({ os, name: targetName(t, locale), rows, state: rows.length ? 'ready' : loaded ? 'failed' : 'loading' })
  }
  return groups
}

/** The links of a card that are not files: the stores and pages of the platforms of its targets, deduplicated by address. `platformOf(target)` gives the platform of the page's config, `partnerOf(target)` a partner's card. */
export function extraLinks(card, { platformOf, partnerOf }) {
  const links = new Map()
  for (const t of card.target ? [card.target] : card.client.targets) {
    for (const link of [...(partnerOf(t)?.links || []), ...(platformOf(t)?.links || [])]) if (!links.has(link.url)) links.set(link.url, link)
  }
  return [...links.values()]
}

/**
 * The state of the popup: no card, no groups (the page lists no file before a click); `open(card)` shows that client's files, `close()` (the close button, Esc and a press on the backdrop all end here)
 * clears it and hands the key of the card to `restoreFocus`, so the card that opened it gets the focus back. The groups follow `assetsOf` and `loadedOf`, so files that arrive late fill the open popup in place.
 */
export function useClientDialog({ assetsOf, loadedOf, locale, restoreFocus }) {
  const card = ref(null)
  const groups = computed(() => (card.value ? fileGroups(card.value, { assets: assetsOf(card.value.client), loaded: loadedOf(card.value.client), locale: locale.value }) : []))
  const open = (next) => { card.value = next }
  const close = () => {
    const key = card.value?.key
    card.value = null
    if (key) restoreFocus?.(key)
  }
  return { card, groups, open, close }
}
